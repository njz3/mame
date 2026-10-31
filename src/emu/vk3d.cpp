// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    vk3d.cpp

    Off-screen Vulkan renderer for high-resolution 3D (mame3d).

    One frame at a time: the recorded batches are rendered into a colour
    and depth target, the colour target is copied into a host-visible
    buffer, and the CPU waits for it before copying it into the bitmap.

***************************************************************************/

#include "emu.h"
#include "vk3d.h"

#include "modules/lib/osdlib.h"

#define VK_NO_PROTOTYPES
#include "bgfx/3rdparty/khronos/vulkan-local/vulkan.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>


namespace {

// shaders compiled with glslc from src/emu/vk3d_shaders (see NOTES.md)
u32 const s_vertex_shared[] = {
#include "vk3d_shaders/vk3d.vert.spv.inc"
};
u32 const s_fragment_color[] = {
#include "vk3d_shaders/color.frag.spv.inc"
};
u32 const s_fragment_stipple[] = {
#include "vk3d_shaders/stipple.frag.spv.inc"
};
u32 const s_fragment_texkey[] = {
#include "vk3d_shaders/texkey.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr u32 MAX_TEXTURES = 256;

} // anonymous namespace


#define VK3D_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr)

#define VK3D_DEVICE_FUNCTIONS(X) \
	X(vkDestroyDevice) \
	X(vkDeviceWaitIdle) \
	X(vkGetDeviceQueue) \
	X(vkQueueSubmit) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkResetCommandBuffer) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkMapMemory) \
	X(vkInvalidateMappedMemoryRanges) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkGetBufferMemoryRequirements) \
	X(vkBindBufferMemory) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdPushConstants) \
	X(vkCmdDraw)


struct vk3d_renderer::impl
{
	struct buffer
	{
		VkBuffer handle = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void *mapped = nullptr;
		VkDeviceSize size = 0;
		bool coherent = true;
	};

	struct image
	{
		VkImage handle = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};

	struct texture
	{
		image img;
		buffer staging;
		VkDescriptorSet set = VK_NULL_HANDLE;
		int width = 0;
		int height = 0;
		bool dirty = true;
		bool initialized = false;
	};

	struct batch
	{
		int pipeline;
		int texture;
		VkRect2D scissor;
		u32 first;
		u32 count;
	};

	osd::dynamic_module::ptr library;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkCreateInstance vkCreateInstance = nullptr;
#define VK3D_DECLARE(f) PFN_##f f = nullptr;
	VK3D_INSTANCE_FUNCTIONS(VK3D_DECLARE)
	VK3D_DEVICE_FUNCTIONS(VK3D_DECLARE)
#undef VK3D_DECLARE

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory_properties{};
	std::string name;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	VkSampler sampler_nearest = VK_NULL_HANDLE;
	VkSampler sampler_linear = VK_NULL_HANDLE;
	VkShaderModule vertex_shared = VK_NULL_HANDLE;
	std::vector<VkShaderModule> modules;
	std::vector<VkPipeline> pipelines;
	std::vector<texture> textures;
	int dummy_texture = -1;

	int width = 0;
	int height = 0;
	image color;
	image depth;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	buffer readback;
	buffer vertices;

	std::vector<vk3d_renderer::vertex> frame_vertices;
	std::vector<batch> batches;
	VkRect2D scissor{};
	VkClearColorValue clear{};

	~impl();

	bool init(std::string &error);
	bool load_instance_functions();
	bool load_device_functions();
	int find_memory(u32 type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const;
	bool create_buffer(buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, bool readback_cached);
	void destroy_buffer(buffer &buf);
	bool create_image(image &img, int w, int h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect);
	void destroy_image(image &img);
	VkShaderModule create_module(u32 const *code, size_t size);
	int create_pipeline(pipeline_desc const &desc);
	bool create_target(int w, int h);
	void destroy_target();
};


//**************************************************************************
//  SETUP
//**************************************************************************

vk3d_renderer::impl::~impl()
{
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		destroy_target();
		destroy_buffer(vertices);
		for (texture &tex : textures)
		{
			destroy_image(tex.img);
			destroy_buffer(tex.staging);
		}
		for (VkPipeline pipeline : pipelines)
			vkDestroyPipeline(device, pipeline, nullptr);
		for (VkShaderModule module : modules)
			vkDestroyShaderModule(device, module, nullptr);
		if (sampler_nearest)
			vkDestroySampler(device, sampler_nearest, nullptr);
		if (sampler_linear)
			vkDestroySampler(device, sampler_linear, nullptr);
		if (descriptor_pool)
			vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
		if (pipeline_layout)
			vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		if (render_pass)
			vkDestroyRenderPass(device, render_pass, nullptr);
		if (fence)
			vkDestroyFence(device, fence, nullptr);
		if (command_pool)
			vkDestroyCommandPool(device, command_pool, nullptr);
		vkDestroyDevice(device, nullptr);
	}
	if (instance && vkDestroyInstance)
		vkDestroyInstance(instance, nullptr);
}


bool vk3d_renderer::impl::load_instance_functions()
{
#define VK3D_LOAD(f) f = reinterpret_cast<PFN_##f>(vkGetInstanceProcAddr(instance, #f)); if (!f) return false;
	VK3D_INSTANCE_FUNCTIONS(VK3D_LOAD)
#undef VK3D_LOAD
	return true;
}


bool vk3d_renderer::impl::load_device_functions()
{
#define VK3D_LOAD(f) f = reinterpret_cast<PFN_##f>(vkGetDeviceProcAddr(device, #f)); if (!f) return false;
	VK3D_DEVICE_FUNCTIONS(VK3D_LOAD)
#undef VK3D_LOAD
	return true;
}


bool vk3d_renderer::impl::init(std::string &error)
{
	library = osd::dynamic_module::open({ "vulkan-1.dll", "libvulkan.so.1", "libvulkan.so", "libvulkan.1.dylib" });
	vkGetInstanceProcAddr = library->bind<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr)
	{
		error = "Vulkan loader not found";
		return false;
	}
	vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
	if (!vkCreateInstance)
	{
		error = "vkCreateInstance not found";
		return false;
	}

	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "mame3d";
	app.pEngineName = "mame3d";
	app.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo instance_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_info.pApplicationInfo = &app;
	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
	{
		error = "vkCreateInstance failed";
		return false;
	}
	if (!load_instance_functions())
	{
		error = "missing Vulkan instance functions";
		return false;
	}

	// pick a device with a graphics queue, preferring a discrete GPU
	u32 count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	u32 queue_family = 0;
	int best_score = -1;
	for (VkPhysicalDevice candidate : devices)
	{
		u32 family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
		for (u32 i = 0; i < family_count; i++)
		{
			if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			VkPhysicalDeviceProperties properties;
			vkGetPhysicalDeviceProperties(candidate, &properties);
			int const score = (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2 :
					(properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
			if (score > best_score)
			{
				best_score = score;
				physical = candidate;
				queue_family = i;
				name = properties.deviceName;
			}
			break;
		}
	}
	if (!physical)
	{
		error = "no Vulkan device with a graphics queue";
		return false;
	}
	vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);

	float const priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queue_info.queueFamilyIndex = queue_family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	VkDeviceCreateInfo device_info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS)
	{
		error = "vkCreateDevice failed";
		return false;
	}
	if (!load_device_functions())
	{
		// the device can only be destroyed with every function loaded
		if (vkDestroyDevice)
			vkDestroyDevice(device, nullptr);
		vkDestroyDevice = nullptr;
		error = "missing Vulkan device functions";
		return false;
	}
	vkGetDeviceQueue(device, queue_family, 0, &queue);

	VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family;
	vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
	VkCommandBufferAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	alloc_info.commandPool = command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;
	vkAllocateCommandBuffers(device, &alloc_info, &command_buffer);
	VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	vkCreateFence(device, &fence_info, nullptr, &fence);

	// render pass: colour kept for the copy-out, depth discarded
	VkAttachmentDescription attachments[2]{};
	attachments[0].format = COLOR_FORMAT;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	attachments[1].format = DEPTH_FORMAT;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkAttachmentReference depth_ref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;
	subpass.pDepthStencilAttachment = &depth_ref;
	VkSubpassDependency dependencies[2]{};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo pass_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	pass_info.attachmentCount = 2;
	pass_info.pAttachments = attachments;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	pass_info.dependencyCount = 2;
	pass_info.pDependencies = dependencies;
	if (vkCreateRenderPass(device, &pass_info, nullptr, &render_pass) != VK_SUCCESS)
	{
		error = "vkCreateRenderPass failed";
		return false;
	}

	// one combined image sampler per draw, a small push constant block
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 1;
	set_info.pBindings = &binding;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
	VkPushConstantRange push_range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
	VkDescriptorPoolSize pool_size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES };
	VkDescriptorPoolCreateInfo descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	descriptor_info.maxSets = MAX_TEXTURES;
	descriptor_info.poolSizeCount = 1;
	descriptor_info.pPoolSizes = &pool_size;
	vkCreateDescriptorPool(device, &descriptor_info, nullptr, &descriptor_pool);

	VkSamplerCreateInfo sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.maxLod = 0.0f;
	vkCreateSampler(device, &sampler_info, nullptr, &sampler_nearest);
	sampler_info.magFilter = VK_FILTER_LINEAR;
	sampler_info.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(device, &sampler_info, nullptr, &sampler_linear);

	vertex_shared = create_module(s_vertex_shared, sizeof(s_vertex_shared));
	if (!vertex_shared)
	{
		error = "vertex shader rejected";
		return false;
	}

	pipeline_desc desc;
	desc.fragment_spirv = s_fragment_color;
	desc.fragment_size = sizeof(s_fragment_color);
	if (create_pipeline(desc) != PIPE_COLOR)
	{
		error = "pipeline creation failed";
		return false;
	}
	desc.fragment_spirv = s_fragment_stipple;
	desc.fragment_size = sizeof(s_fragment_stipple);
	if (create_pipeline(desc) != PIPE_COLOR_STIPPLE)
	{
		error = "pipeline creation failed";
		return false;
	}
	desc.fragment_spirv = s_fragment_texkey;
	desc.fragment_size = sizeof(s_fragment_texkey);
	if (create_pipeline(desc) != PIPE_TEXTURE_KEY)
	{
		error = "pipeline creation failed";
		return false;
	}

	if (!create_buffer(vertices, 1 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false))
	{
		error = "vertex buffer allocation failed";
		return false;
	}
	return true;
}


int vk3d_renderer::impl::find_memory(u32 type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const
{
	int fallback = -1;
	for (u32 i = 0; i < memory_properties.memoryTypeCount; i++)
	{
		VkMemoryPropertyFlags const flags = memory_properties.memoryTypes[i].propertyFlags;
		if (!(type_bits & (1U << i)) || ((flags & required) != required))
			continue;
		if ((flags & preferred) == preferred)
			return i;
		if (fallback < 0)
			fallback = i;
	}
	return fallback;
}


bool vk3d_renderer::impl::create_buffer(buffer &buf, VkDeviceSize size, VkBufferUsageFlags usage, bool readback_cached)
{
	VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &info, nullptr, &buf.handle) != VK_SUCCESS)
		return false;
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buf.handle, &requirements);

	// host writes want coherent memory; the read-back is much faster cached
	VkMemoryPropertyFlags const required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	VkMemoryPropertyFlags const preferred = readback_cached
			? (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
			: VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	int const type = find_memory(requirements.memoryTypeBits, required, preferred);
	if (type < 0)
		return false;
	buf.coherent = (memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = requirements.size;
	alloc.memoryTypeIndex = type;
	if (vkAllocateMemory(device, &alloc, nullptr, &buf.memory) != VK_SUCCESS)
		return false;
	vkBindBufferMemory(device, buf.handle, buf.memory, 0);
	if (vkMapMemory(device, buf.memory, 0, VK_WHOLE_SIZE, 0, &buf.mapped) != VK_SUCCESS)
		return false;
	buf.size = size;
	return true;
}


void vk3d_renderer::impl::destroy_buffer(buffer &buf)
{
	if (buf.handle)
		vkDestroyBuffer(device, buf.handle, nullptr);
	if (buf.memory)
		vkFreeMemory(device, buf.memory, nullptr); // unmaps it
	buf = buffer();
}


bool vk3d_renderer::impl::create_image(image &img, int w, int h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = { u32(w), u32(h), 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(device, &info, nullptr, &img.handle) != VK_SUCCESS)
		return false;
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(device, img.handle, &requirements);
	int const type = find_memory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0)
		return false;
	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = requirements.size;
	alloc.memoryTypeIndex = type;
	if (vkAllocateMemory(device, &alloc, nullptr, &img.memory) != VK_SUCCESS)
		return false;
	vkBindImageMemory(device, img.handle, img.memory, 0);
	VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	view.image = img.handle;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = format;
	view.subresourceRange = { aspect, 0, 1, 0, 1 };
	return vkCreateImageView(device, &view, nullptr, &img.view) == VK_SUCCESS;
}


void vk3d_renderer::impl::destroy_image(image &img)
{
	if (img.view)
		vkDestroyImageView(device, img.view, nullptr);
	if (img.handle)
		vkDestroyImage(device, img.handle, nullptr);
	if (img.memory)
		vkFreeMemory(device, img.memory, nullptr);
	img = image();
}


VkShaderModule vk3d_renderer::impl::create_module(u32 const *code, size_t size)
{
	VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	info.codeSize = size;
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	modules.push_back(module);
	return module;
}


int vk3d_renderer::impl::create_pipeline(pipeline_desc const &desc)
{
	VkShaderModule const vertex = desc.vertex_spirv ? create_module(desc.vertex_spirv, desc.vertex_size) : vertex_shared;
	VkShaderModule const fragment = create_module(desc.fragment_spirv, desc.fragment_size);
	if (!vertex || !fragment)
		return -1;

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";

	VkVertexInputBindingDescription vertex_binding{ 0, sizeof(vk3d_renderer::vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription vertex_attributes[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk3d_renderer::vertex, x) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk3d_renderer::vertex, r) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk3d_renderer::vertex, u) } };
	VkPipelineVertexInputStateCreateInfo vertex_input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 3;
	vertex_input.pVertexAttributeDescriptions = vertex_attributes;

	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
	depth.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState blend{};
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (desc.blend != BLEND_NONE)
	{
		blend.blendEnable = VK_TRUE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = (desc.blend == BLEND_ADD) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo blending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blending.attachmentCount = 1;
	blending.pAttachments = &blend;
	VkDynamicState const dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;

	VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depth;
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = pipeline_layout;
	info.renderPass = render_pass;
	VkPipeline pipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
		return -1;
	pipelines.push_back(pipeline);
	return int(pipelines.size()) - 1;
}


bool vk3d_renderer::impl::create_target(int w, int h)
{
	destroy_target();
	if (!create_image(color, w, h, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
		return false;
	if (!create_image(depth, w, h, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
		return false;
	VkImageView const views[2] = { color.view, depth.view };
	VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = render_pass;
	info.attachmentCount = 2;
	info.pAttachments = views;
	info.width = w;
	info.height = h;
	info.layers = 1;
	if (vkCreateFramebuffer(device, &info, nullptr, &framebuffer) != VK_SUCCESS)
		return false;
	if (!create_buffer(readback, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
		return false;
	width = w;
	height = h;
	return true;
}


void vk3d_renderer::impl::destroy_target()
{
	if (framebuffer)
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	framebuffer = VK_NULL_HANDLE;
	destroy_image(color);
	destroy_image(depth);
	destroy_buffer(readback);
	width = height = 0;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

std::unique_ptr<vk3d_renderer> vk3d_renderer::create(std::string &error)
{
	auto state = std::make_unique<impl>();
	if (!state->init(error))
		return nullptr;
	std::unique_ptr<vk3d_renderer> renderer(new vk3d_renderer(std::move(state)));

	// bound to draws without a texture
	renderer->m->dummy_texture = renderer->create_texture(1, 1);
	if (renderer->m->dummy_texture < 0)
	{
		error = "texture allocation failed";
		return nullptr;
	}
	return renderer;
}


vk3d_renderer::vk3d_renderer(std::unique_ptr<impl> &&state)
	: m(std::move(state))
{
}


vk3d_renderer::~vk3d_renderer()
{
}


std::string const &vk3d_renderer::device_name() const
{
	return m->name;
}


int vk3d_renderer::add_pipeline(pipeline_desc const &desc)
{
	return m->create_pipeline(desc);
}


int vk3d_renderer::create_texture(int width, int height, bool linear_filter)
{
	if (m->textures.size() >= MAX_TEXTURES)
		return -1;
	impl::texture tex;
	tex.width = width;
	tex.height = height;
	if (!m->create_image(tex.img, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!m->create_buffer(tex.staging, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
	{
		m->destroy_image(tex.img);
		m->destroy_buffer(tex.staging);
		return -1;
	}

	VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorPool = m->descriptor_pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &m->set_layout;
	m->vkAllocateDescriptorSets(m->device, &alloc, &tex.set);
	VkDescriptorImageInfo image_info{ linear_filter ? m->sampler_linear : m->sampler_nearest, tex.img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = tex.set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	m->vkUpdateDescriptorSets(m->device, 1, &write, 0, nullptr);

	std::memset(tex.staging.mapped, 0, size_t(width) * height * 4);
	m->textures.push_back(tex);
	return int(m->textures.size()) - 1;
}


void vk3d_renderer::update_texture(int texture, bitmap_rgb32 const &source, rectangle const &area)
{
	impl::texture &tex = m->textures[texture];
	rectangle clip = area;
	clip &= rectangle(0, std::min(tex.width, source.width()) - 1, 0, std::min(tex.height, source.height()) - 1);
	u32 *const base = reinterpret_cast<u32 *>(tex.staging.mapped);
	for (int y = clip.top(); y <= clip.bottom(); y++)
		std::memcpy(base + size_t(y) * tex.width + clip.left(), &source.pix(y, clip.left()), size_t(clip.width()) * 4);
	tex.dirty = true;
}


void vk3d_renderer::begin_frame(int width, int height, rgb_t clear)
{
	if ((width != m->width) || (height != m->height))
	{
		m->vkDeviceWaitIdle(m->device);
		m->create_target(width, height);
	}
	m->frame_vertices.clear();
	m->batches.clear();
	m->scissor = { { 0, 0 }, { u32(width), u32(height) } };
	m->clear.float32[0] = clear.r() / 255.0f;
	m->clear.float32[1] = clear.g() / 255.0f;
	m->clear.float32[2] = clear.b() / 255.0f;
	m->clear.float32[3] = 1.0f;
}


void vk3d_renderer::set_scissor(rectangle const &clip)
{
	int const x0 = std::clamp(clip.left(), 0, m->width);
	int const y0 = std::clamp(clip.top(), 0, m->height);
	int const x1 = std::clamp(clip.right() + 1, x0, m->width);
	int const y1 = std::clamp(clip.bottom() + 1, y0, m->height);
	m->scissor = { { x0, y0 }, { u32(x1 - x0), u32(y1 - y0) } };
}


void vk3d_renderer::draw(int pipeline, int texture, vertex const *vertices, size_t count)
{
	if (!count || !m->scissor.extent.width || !m->scissor.extent.height)
		return;
	if (texture < 0)
		texture = m->dummy_texture;
	u32 const first = u32(m->frame_vertices.size());
	m->frame_vertices.insert(m->frame_vertices.end(), vertices, vertices + count);

	// extend the previous batch when nothing changes in between
	if (!m->batches.empty())
	{
		impl::batch &last = m->batches.back();
		if ((last.pipeline == pipeline) && (last.texture == texture) &&
				(last.first + last.count == first) &&
				!std::memcmp(&last.scissor, &m->scissor, sizeof(VkRect2D)))
		{
			last.count += u32(count);
			return;
		}
	}
	m->batches.push_back({ pipeline, texture, m->scissor, first, u32(count) });
}


void vk3d_renderer::draw_rect(int pipeline, int texture, float x0, float y0, float x1, float y1, rgb_t color)
{
	float const r = color.r() / 255.0f, g = color.g() / 255.0f, b = color.b() / 255.0f;
	vertex const v[6] = {
		{ x0, y0, 0, 1, r, g, b, 1, 0, 0, 1, 0 },
		{ x1, y0, 0, 1, r, g, b, 1, 1, 0, 1, 0 },
		{ x1, y1, 0, 1, r, g, b, 1, 1, 1, 1, 0 },
		{ x0, y0, 0, 1, r, g, b, 1, 0, 0, 1, 0 },
		{ x1, y1, 0, 1, r, g, b, 1, 1, 1, 1, 0 },
		{ x0, y1, 0, 1, r, g, b, 1, 0, 1, 1, 0 } };
	draw(pipeline, texture, v, 6);
}


bool vk3d_renderer::end_frame(bitmap_rgb32 &dest, rectangle const &clip)
{
	impl &s = *m;
	if (!s.framebuffer)
		return false;

	// vertex data
	VkDeviceSize const vertex_bytes = s.frame_vertices.size() * sizeof(vertex);
	if (vertex_bytes > s.vertices.size)
	{
		s.destroy_buffer(s.vertices);
		if (!s.create_buffer(s.vertices, std::max<VkDeviceSize>(vertex_bytes * 2, 1 << 20), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false))
			return false;
	}
	if (vertex_bytes)
		std::memcpy(s.vertices.mapped, s.frame_vertices.data(), vertex_bytes);

	s.vkResetCommandBuffer(s.command_buffer, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	s.vkBeginCommandBuffer(s.command_buffer, &begin);
	VkCommandBuffer const cmd = s.command_buffer;

	// texture uploads
	for (impl::texture &tex : s.textures)
	{
		if (!tex.dirty)
			continue;
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = tex.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = tex.initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = tex.img.handle;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { u32(tex.width), u32(tex.height), 1 };
		s.vkCmdCopyBufferToImage(cmd, tex.staging.handle, tex.img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		tex.dirty = false;
		tex.initialized = true;
	}

	// draws
	VkClearValue clears[2];
	clears[0].color = s.clear;
	clears[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = s.render_pass;
	pass.framebuffer = s.framebuffer;
	pass.renderArea = { { 0, 0 }, { u32(s.width), u32(s.height) } };
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	s.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport const viewport{ 0.0f, 0.0f, float(s.width), float(s.height), 0.0f, 1.0f };
	s.vkCmdSetViewport(cmd, 0, 1, &viewport);
	float const push[4] = { 1.0f / s.width, 1.0f / s.height, 0.0f, 0.0f };
	s.vkCmdPushConstants(cmd, s.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
	if (vertex_bytes)
	{
		VkDeviceSize const offset = 0;
		s.vkCmdBindVertexBuffers(cmd, 0, 1, &s.vertices.handle, &offset);
	}
	int bound_pipeline = -1;
	int bound_texture = -1;
	for (impl::batch const &b : s.batches)
	{
		if (b.pipeline != bound_pipeline)
		{
			s.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipelines[b.pipeline]);
			bound_pipeline = b.pipeline;
		}
		if (b.texture != bound_texture)
		{
			s.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipeline_layout, 0, 1, &s.textures[b.texture].set, 0, nullptr);
			bound_texture = b.texture;
		}
		s.vkCmdSetScissor(cmd, 0, 1, &b.scissor);
		s.vkCmdDraw(cmd, b.count, 1, b.first, 0);
	}
	s.vkCmdEndRenderPass(cmd);

	// copy out (the render pass leaves the colour in TRANSFER_SRC layout)
	VkBufferImageCopy copy{};
	copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageExtent = { u32(s.width), u32(s.height), 1 };
	s.vkCmdCopyImageToBuffer(cmd, s.color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readback.handle, 1, &copy);
	VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = s.readback.handle;
	host.size = VK_WHOLE_SIZE;
	s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	s.vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	if (s.vkQueueSubmit(s.queue, 1, &submit, s.fence) != VK_SUCCESS)
		return false;
	s.vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, ~u64(0));
	s.vkResetFences(s.device, 1, &s.fence);

	if (!s.readback.coherent)
	{
		VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		range.memory = s.readback.memory;
		range.size = VK_WHOLE_SIZE;
		s.vkInvalidateMappedMemoryRanges(s.device, 1, &range);
	}
	rectangle area = clip;
	area &= rectangle(0, std::min(s.width, dest.width()) - 1, 0, std::min(s.height, dest.height()) - 1);
	u32 const *const base = reinterpret_cast<u32 const *>(s.readback.mapped);
	for (int y = area.top(); y <= area.bottom(); y++)
		std::memcpy(&dest.pix(y, area.left()), base + size_t(y) * s.width + area.left(), size_t(area.width()) * 4);
	return true;
}
