// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    namcos22_vk.cpp

    Vulkan renderer for Namco System 22 / Super System 22 at a higher
    resolution (mame3d).

    One frame at a time: the polygons and sprites are recorded on the CPU
    (a record per polygon, its vertices as triangles), then end_frame()
    uploads them with the palette, the native text layer and the mix
    parameters, draws the scene into a colour image and a priority image,
    mixes the text layer into the display image, and reads it back.

***************************************************************************/

#include "emu.h"
#include "namcos22_vk.h"

#include "vk3d_context.h"
#include "vk3d_share.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

#include "namcos22_vk_shaders/layout.h"

// shaders compiled with glslc from namcos22_vk_shaders (see NOTES.md)
u32 const s_scene_vert[] = {
#include "namcos22_vk_shaders/scene.vert.spv.inc"
};
u32 const s_scene_frag[] = {
#include "namcos22_vk_shaders/scene.frag.spv.inc"
};
u32 const s_mix_vert[] = {
#include "namcos22_vk_shaders/mix.vert.spv.inc"
};
u32 const s_mix_frag[] = {
#include "namcos22_vk_shaders/mix.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat PRI_FORMAT = VK_FORMAT_R8_UINT;
constexpr size_t TILEDATA_BYTES = 0x1000000; // the texel index has 24 bits
constexpr int READBACKS = 3;
constexpr int SLOTS = 2;            // frames in flight

struct push_constants
{
	float inv_width, inv_height;
	s32 scale, pass;
	u32 vertices, records, palette, text, textpri, params, gamma, spot;
	s32 native_width;
};

struct batch
{
	u32 first;
	u32 count;
	VkRect2D scissor;
};

} // anonymous namespace


struct namcos22_vk::impl : vk3d_context
{
	struct readback_entry
	{
		buffer host;
		bitmap_rgb32 view;
		int slot = -1;          // frame that copies it
		bool ready = true;
	};

	// a frame in flight: its commands, fence and uploaded data
	struct slot
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool busy = false;
		buffer host;
	};

	VkRenderPass scene_pass = VK_NULL_HANDLE;
	VkRenderPass mix_pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layouts[2]{};
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet sets[2]{};
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipeline scene_pipeline = VK_NULL_HANDLE;
	VkPipeline before_pipeline = VK_NULL_HANDLE;
	VkPipeline final_pipeline = VK_NULL_HANDLE;

	// targets
	rectangle visible;
	int scale = 1;
	int width = 0;
	int height = 0;
	image color;
	image pri;
	image display;
	VkFramebuffer scene_framebuffer = VK_NULL_HANDLE;
	VkFramebuffer display_framebuffer = VK_NULL_HANDLE;
	readback_entry readback[READBACKS];
	int next_readback = 0;
	slot slots[SLOTS];
	int next_slot = 0;

	// statistics, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u32 frames = 0;

	// textures of the display the frames are copied into instead
	std::unique_ptr<vk3d_share> share;

	// ROM data, then the data of each frame
	std::vector<u32> texture_data;
	std::vector<u32> sprite_data;
	buffer texture;
	buffer sprite;
	buffer frame_device;
	bool rom_uploaded = false;
	bool descriptors_dirty = true;

	// the frame being recorded
	rgb_t background;
	std::vector<u32> records;
	std::vector<u32> vertices;
	std::vector<batch> batches;

	~impl();

	bool init(std::string &error);
	VkPipeline make_pipeline(VkShaderModule vertex, VkShaderModule fragment, VkRenderPass pass, int attachments, bool blend, bool pri_write);
	bool create_targets();
	void destroy_targets();
	bool ensure_frame(slot &target, VkDeviceSize bytes);
	void update_descriptors();
	void wait_slot(slot &target);
	void wait_all();
};


//**************************************************************************
//  SETUP
//**************************************************************************

namcos22_vk::impl::~impl()
{
	if (frames)
		osd_printf_verbose("namcos22_vk: %u frames, %.1f ms waiting for the GPU\n", frames, wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		destroy_targets();
		destroy_buffer(texture);
		destroy_buffer(sprite);
		for (slot &target : slots)
		{
			destroy_buffer(target.host);
			if (target.fence)
				vkDestroyFence(device, target.fence, nullptr);
		}
		destroy_buffer(frame_device);
		for (VkPipeline p : { scene_pipeline, before_pipeline, final_pipeline })
			if (p)
				vkDestroyPipeline(device, p, nullptr);
		if (layout)
			vkDestroyPipelineLayout(device, layout, nullptr);
		if (pool)
			vkDestroyDescriptorPool(device, pool, nullptr);
		for (VkDescriptorSetLayout l : set_layouts)
			if (l)
				vkDestroyDescriptorSetLayout(device, l, nullptr);
		if (sampler)
			vkDestroySampler(device, sampler, nullptr);
		for (VkRenderPass p : { scene_pass, mix_pass })
			if (p)
				vkDestroyRenderPass(device, p, nullptr);
	}
}


bool namcos22_vk::impl::init(std::string &error)
{
	// the priority image is written without blending while the colour is blended
	VkPhysicalDeviceFeatures wanted{};
	wanted.independentBlend = VK_TRUE;
	if (!init_device(error, &wanted))
		return false;
	if (!enabled_features().independentBlend)
	{
		error = "the GPU lacks independentBlend";
		return false;
	}
	share = std::make_unique<vk3d_share>(*this);

	// scene pass: colour and priority, sampled by the mix pass afterwards
	{
		VkAttachmentDescription attachments[2]{};
		for (int i = 0; i < 2; i++)
		{
			attachments[i].format = i ? PRI_FORMAT : COLOR_FORMAT;
			attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			attachments[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		VkAttachmentReference refs[2] = { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 2;
		subpass.pColorAttachments = refs;
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 2;
		info.pAttachments = attachments;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = 2;
		info.pDependencies = dependencies;
		if (vkCreateRenderPass(device, &info, nullptr, &scene_pass) != VK_SUCCESS)
		{
			error = "vkCreateRenderPass failed";
			return false;
		}
	}

	// mix pass: the display image, left ready for the copy
	{
		VkAttachmentDescription attachment{};
		attachment.format = COLOR_FORMAT;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &ref;
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 1;
		info.pAttachments = &attachment;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = 2;
		info.pDependencies = dependencies;
		if (vkCreateRenderPass(device, &info, nullptr, &mix_pass) != VK_SUCCESS)
		{
			error = "vkCreateRenderPass failed";
			return false;
		}
	}

	// set 0: ROM, sprite and frame data; set 1: the scene images
	VkDescriptorSetLayoutBinding buffers[3]{};
	for (int i = 0; i < 3; i++)
	{
		buffers[i].binding = i;
		buffers[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		buffers[i].descriptorCount = 1;
		buffers[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutBinding images[2]{};
	for (int i = 0; i < 2; i++)
	{
		images[i].binding = i;
		images[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		images[i].descriptorCount = 1;
		images[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 3;
	set_info.pBindings = buffers;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts[0]);
	set_info.bindingCount = 2;
	set_info.pBindings = images;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts[1]);
	VkPushConstantRange push_range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants) };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 2;
	layout_info.pSetLayouts = set_layouts;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
	VkDescriptorPoolSize pool_sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
	VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pool_info.maxSets = 2;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = pool_sizes;
	vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
	VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorPool = pool;
	alloc.descriptorSetCount = 2;
	alloc.pSetLayouts = set_layouts;
	if (vkAllocateDescriptorSets(device, &alloc, sets) != VK_SUCCESS)
	{
		error = "descriptor set allocation failed";
		return false;
	}
	VkSamplerCreateInfo sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	vkCreateSampler(device, &sampler_info, nullptr, &sampler);

	VkShaderModule const scene_vert = create_module(s_scene_vert, sizeof(s_scene_vert));
	VkShaderModule const scene_frag = create_module(s_scene_frag, sizeof(s_scene_frag));
	VkShaderModule const mix_vert = create_module(s_mix_vert, sizeof(s_mix_vert));
	VkShaderModule const mix_frag = create_module(s_mix_frag, sizeof(s_mix_frag));
	if (!scene_vert || !scene_frag || !mix_vert || !mix_frag)
	{
		error = "shader rejected";
		return false;
	}
	scene_pipeline = make_pipeline(scene_vert, scene_frag, scene_pass, 2, true, true);
	before_pipeline = make_pipeline(mix_vert, mix_frag, scene_pass, 2, false, false);
	final_pipeline = make_pipeline(mix_vert, mix_frag, mix_pass, 1, false, false);
	if (!scene_pipeline || !before_pipeline || !final_pipeline)
	{
		error = "pipeline creation failed";
		return false;
	}

	for (slot &target : slots)
	{
		VkCommandBufferAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		alloc_info.commandPool = command_pool;
		alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc_info.commandBufferCount = 1;
		VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		if ((vkAllocateCommandBuffers(device, &alloc_info, &target.cmd) != VK_SUCCESS) ||
				(vkCreateFence(device, &fence_info, nullptr, &target.fence) != VK_SUCCESS))
		{
			error = "command buffer allocation failed";
			return false;
		}
	}
	return true;
}


VkPipeline namcos22_vk::impl::make_pipeline(VkShaderModule vertex, VkShaderModule fragment, VkRenderPass pass, int attachments, bool blend, bool pri_write)
{
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vertex_input{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// colour: blended against the destination by the fragment alpha;
	// priority: written as is, or not at all
	VkPipelineColorBlendAttachmentState states[2]{};
	states[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (blend)
	{
		states[0].blendEnable = VK_TRUE;
		states[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		states[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		states[0].colorBlendOp = VK_BLEND_OP_ADD;
		states[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		states[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		states[0].alphaBlendOp = VK_BLEND_OP_ADD;
	}
	states[1].colorWriteMask = pri_write ? VK_COLOR_COMPONENT_R_BIT : 0;
	VkPipelineColorBlendStateCreateInfo blending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blending.attachmentCount = attachments;
	blending.pAttachments = states;
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
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = layout;
	info.renderPass = pass;
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return result;
}


bool namcos22_vk::impl::create_targets()
{
	VkImageUsageFlags const sampled = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!create_image(color, width, height, COLOR_FORMAT, sampled, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_image(pri, width, height, PRI_FORMAT, sampled, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_image(display, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
		return false;

	VkImageView const views[2] = { color.view, pri.view };
	VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = scene_pass;
	info.attachmentCount = 2;
	info.pAttachments = views;
	info.width = width;
	info.height = height;
	info.layers = 1;
	if (vkCreateFramebuffer(device, &info, nullptr, &scene_framebuffer) != VK_SUCCESS)
		return false;
	info.renderPass = mix_pass;
	info.attachmentCount = 1;
	info.pAttachments = &display.view;
	if (vkCreateFramebuffer(device, &info, nullptr, &display_framebuffer) != VK_SUCCESS)
		return false;

	for (readback_entry &entry : readback)
		if (!create_buffer(entry.host, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			return false;

	// the mix pass samples the scene images
	VkDescriptorImageInfo image_infos[2] = {
			{ sampler, color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ sampler, pri.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
	VkWriteDescriptorSet writes[2]{};
	for (int i = 0; i < 2; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = sets[1];
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &image_infos[i];
	}
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	return true;
}


void namcos22_vk::impl::destroy_targets()
{
	for (VkFramebuffer *fb : { &scene_framebuffer, &display_framebuffer })
	{
		if (*fb)
			vkDestroyFramebuffer(device, *fb, nullptr);
		*fb = VK_NULL_HANDLE;
	}
	destroy_image(color);
	destroy_image(pri);
	destroy_image(display);
	for (readback_entry &entry : readback)
	{
		destroy_buffer(entry.host);
		entry.slot = -1;
		entry.ready = true;
	}
}


bool namcos22_vk::impl::ensure_frame(slot &target, VkDeviceSize bytes)
{
	VkDeviceSize const size = std::max<VkDeviceSize>(bytes * 3 / 2, 1 << 20);
	if (frame_device.size < bytes)
	{
		// the frames in flight read it
		wait_all();
		destroy_buffer(frame_device);
		descriptors_dirty = true;
		if (!create_device_buffer(frame_device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
			return false;
	}
	if (target.host.size < bytes)
	{
		destroy_buffer(target.host);
		if (!create_buffer(target.host, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
			return false;
	}
	return true;
}


void namcos22_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void namcos22_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


void namcos22_vk::impl::update_descriptors()
{
	buffer const *const sources[3] = { &texture, &sprite, &frame_device };
	VkDescriptorBufferInfo infos[3];
	VkWriteDescriptorSet writes[3]{};
	for (int i = 0; i < 3; i++)
	{
		infos[i] = { sources[i]->handle, 0, VK_WHOLE_SIZE };
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = sets[0];
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &infos[i];
	}
	vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
	descriptors_dirty = false;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

std::unique_ptr<namcos22_vk> namcos22_vk::create(std::string &error)
{
	auto state = std::make_unique<impl>();
	if (!state->init(error))
		return nullptr;
	return std::unique_ptr<namcos22_vk>(new namcos22_vk(std::move(state)));
}


namcos22_vk::namcos22_vk(std::unique_ptr<impl> &&state)
	: m(std::move(state))
{
}


namcos22_vk::~namcos22_vk()
{
}


std::string const &namcos22_vk::device_name() const
{
	return m->name;
}


void namcos22_vk::set_texture_memory(u16 const *tilemap, u8 const *tileattr, u8 const *ayx, u8 const *tiledata, size_t tiledata_bytes)
{
	std::vector<u32> &data = m->texture_data;
	data.assign(NT_TILEDATA + TILEDATA_BYTES / 4, 0);
	u8 *const bytes = reinterpret_cast<u8 *>(data.data());
	std::memcpy(bytes + NT_TILEMAP * 4, tilemap, 0x100000 * 2);
	std::memcpy(bytes + NT_TILEATTR * 4, tileattr, 0x100000);
	std::memcpy(bytes + NT_AYX * 4, ayx, 0x1000);
	std::memcpy(bytes + NT_TILEDATA * 4, tiledata, std::min(tiledata_bytes, TILEDATA_BYTES));
	m->rom_uploaded = false;
}


void namcos22_vk::set_sprite_memory(u8 const *data, size_t bytes)
{
	m->sprite_data.assign((std::max<size_t>(bytes, 4) + 3) / 4, 0);
	std::memcpy(m->sprite_data.data(), data, bytes);
	m->rom_uploaded = false;
}


void namcos22_vk::begin_frame(rectangle const &visible, int scale, rgb_t background)
{
	impl &s = *m;
	int const width = visible.width() * scale;
	int const height = visible.height() * scale;
	if ((width != s.width) || (height != s.height) || (scale != s.scale))
	{
		s.vkDeviceWaitIdle(s.device);
		s.destroy_targets();
		s.width = width;
		s.height = height;
		s.scale = scale;
		if (!s.create_targets())
		{
			s.destroy_targets();
			s.width = s.height = 0;
		}
	}
	s.visible = visible;
	s.background = background;
	s.records.clear();
	s.vertices.clear();
	s.batches.clear();
}


void namcos22_vk::polygon(draw const &d, vertex const *v, int count, rectangle const &clip)
{
	impl &s = *m;
	if ((count < 3) || !s.width)
		return;

	// the record
	u32 const record = u32(s.records.size());
	u32 words[NR_SIZE]{};
	words[NR_KIND] = (d.kind == KIND_SPRITE) ? NRK_SPRITE : (d.kind == KIND_POLY_SUPER) ? NRK_POLY_SUPER : NRK_POLY;
	words[NR_PENBASE] = d.penbase;
	words[NR_PENMASK] = d.penmask;
	words[NR_PENSHIFT] = d.penshift;
	words[NR_BANK] = d.bank;
	words[NR_FLAGS] = (d.shade ? NRF_SHADE : 0) | (d.texture ? NRF_TEXTURE : 0) | (d.alpha_all ? NRF_ALPHA_ALL : 0) | (d.polyfade ? NRF_POLYFADE : 0);
	words[NR_FOGFACTOR] = u32(d.fogfactor);
	words[NR_FOGCOLOR] = d.fogcolor & 0xffffff;
	words[NR_FADEFACTOR] = u32(d.fadefactor);
	words[NR_FADECOLOR] = d.fadecolor & 0xffffff;
	words[NR_POLYCOLOR] = d.polycolor & 0xffffff;
	words[NR_ALPHA] = u32(d.alpha);
	words[NR_ALPHAPEN] = d.alphapen;
	words[NR_PRIO] = d.prio;
	words[NR_SOURCE] = d.source;
	words[NR_MODULO] = d.modulo;
	words[NR_FLIPX] = u32(d.flipx);
	words[NR_FLIPY] = u32(d.flipy);
	s.records.insert(s.records.end(), std::begin(words), std::end(words));

	// the fan as triangles, in target pixels
	float const ox = float(s.visible.left() * s.scale);
	float const oy = float(s.visible.top() * s.scale);
	u32 const first = u32(s.vertices.size() / NV_SIZE);
	auto const put = [&] (vertex const &vert)
	{
		float const values[6] = { vert.x - ox, vert.y - oy, vert.p[0], vert.p[1], vert.p[2], vert.p[3] };
		for (float value : values)
		{
			u32 bits;
			std::memcpy(&bits, &value, 4);
			s.vertices.push_back(bits);
		}
		s.vertices.push_back(record);
		s.vertices.push_back(0);
	};
	for (int i = 1; i + 1 < count; i++)
	{
		put(v[0]);
		put(v[i]);
		put(v[i + 1]);
	}
	u32 const vcount = u32(3 * (count - 2));

	// scissor in target pixels
	s32 const left = std::clamp<s32>(clip.left() - s.visible.left() * s.scale, 0, s.width);
	s32 const right = std::clamp<s32>(clip.right() + 1 - s.visible.left() * s.scale, left, s.width);
	s32 const top = std::clamp<s32>(clip.top() - s.visible.top() * s.scale, 0, s.height);
	s32 const bottom = std::clamp<s32>(clip.bottom() + 1 - s.visible.top() * s.scale, top, s.height);
	VkRect2D const scissor{ { left, top }, { u32(right - left), u32(bottom - top) } };
	if (!s.batches.empty())
	{
		batch &last = s.batches.back();
		if ((last.first + last.count == first) && !std::memcmp(&last.scissor, &scissor, sizeof(scissor)))
		{
			last.count += vcount;
			return;
		}
	}
	s.batches.push_back({ first, vcount, scissor });
}


bitmap_rgb32 *namcos22_vk::end_frame(pen_t const *palette, size_t entries, bitmap_ind16 const &text, bitmap_ind8 const &textpri, mix_params const &mix)
{
	impl &s = *m;
	if (!s.width)
		return nullptr;

	// the slot of two frames ago is free once its commands are done
	int const slot_index = s.next_slot;
	impl::slot &sl = s.slots[slot_index];
	s.next_slot = (s.next_slot + 1) % SLOTS;
	s.wait_slot(sl);

	// the frame data: records, vertices, parameters, gamma, spot RAM,
	// palette, text layer and priority map of the visible area
	int const nwidth = s.visible.width();
	int const nheight = s.visible.height();
	u32 const records = 0;
	u32 const vertices = records + u32(s.records.size());
	u32 const params = vertices + u32(s.vertices.size());
	u32 const gamma = params + NM_SIZE;
	u32 const spot = gamma + 3 * 256;
	u32 const pal = spot + 0x800 / 2;
	u32 const textw = pal + u32(entries);
	u32 const priw = textw + u32((nwidth * nheight + 1) / 2);
	u32 const total = priw + u32((nwidth * nheight + 3) / 4);
	if (!s.ensure_frame(sl, VkDeviceSize(total) * 4))
		return nullptr;

	u32 *const out = reinterpret_cast<u32 *>(sl.host.mapped);
	std::copy(s.records.begin(), s.records.end(), out + records);
	std::copy(s.vertices.begin(), s.vertices.end(), out + vertices);
	u32 *const p = out + params;
	std::fill_n(p, NM_SIZE, 0);
	p[NM_SUPER] = mix.super;
	p[NM_BACKGROUND] = mix.background & 0xffffff;
	p[NM_FADE_ON] = mix.fade_on;
	p[NM_FADE_R] = mix.fade_r;
	p[NM_FADE_G] = mix.fade_g;
	p[NM_FADE_B] = mix.fade_b;
	p[NM_FADE_FACTOR] = mix.fade_factor;
	p[NM_SHADOW_ON] = mix.shadow_on;
	for (int i = 0; i < 3; i++)
		p[NM_MIX0 + i] = mix.mix[i] & 0xffffff;
	p[NM_ALPHA_CHECK12] = mix.alpha_check12;
	p[NM_ALPHA_CHECK13] = mix.alpha_check13;
	p[NM_ALPHA_MASK] = mix.alpha_mask;
	p[NM_ALPHA_FACTOR] = mix.alpha_factor;
	p[NM_SPOT_ON] = mix.spot_on;
	p[NM_SPOT_FACTOR] = mix.spot_factor;
	p[NM_SPOT_PALBASE] = mix.spot_palbase;
	p[NM_TEXT_PALBASE] = mix.text_palbase;
	for (int c = 0; c < 3; c++)
		for (int i = 0; i < 256; i++)
			out[gamma + c * 256 + i] = mix.gamma[c][i];
	if (mix.spotram)
		std::memcpy(out + spot, mix.spotram, 0x800 * 2);
	else
		std::fill_n(out + spot, 0x800 / 2, 0);
	for (size_t i = 0; i < entries; i++)
		out[pal + i] = palette[i];
	u16 *const text16 = reinterpret_cast<u16 *>(out + textw);
	u8 *const pri8 = reinterpret_cast<u8 *>(out + priw);
	for (int y = 0; y < nheight; y++)
	{
		std::memcpy(&text16[y * nwidth], &text.pix(s.visible.top() + y, s.visible.left()), nwidth * 2);
		std::memcpy(&pri8[y * nwidth], &textpri.pix(s.visible.top() + y, s.visible.left()), nwidth);
	}
	s.flush_buffer(sl.host, 0, VkDeviceSize(total) * 4);

	// the ROM data, once
	vk3d_context::buffer staging_texture, staging_sprite;
	bool const upload_rom = !s.rom_uploaded;
	if (upload_rom)
	{
		VkDeviceSize const texture_bytes = s.texture_data.size() * 4;
		VkDeviceSize const sprite_bytes = std::max<size_t>(s.sprite_data.size(), 1) * 4;
		s.destroy_buffer(s.texture);
		s.destroy_buffer(s.sprite);
		if (!s.create_device_buffer(s.texture, texture_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
				!s.create_device_buffer(s.sprite, sprite_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
				!s.create_buffer(staging_texture, texture_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) ||
				!s.create_buffer(staging_sprite, sprite_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
			return nullptr;
		std::memcpy(staging_texture.mapped, s.texture_data.data(), texture_bytes);
		std::memcpy(staging_sprite.mapped, s.sprite_data.data(), s.sprite_data.size() * 4);
		s.flush_buffer(staging_texture, 0, texture_bytes);
		s.flush_buffer(staging_sprite, 0, sprite_bytes);
		s.descriptors_dirty = true;
	}
	if (s.descriptors_dirty)
	{
		// the sets may not change while a frame in flight uses them
		s.wait_all();
		s.update_descriptors();
	}

	VkCommandBuffer const cmd = sl.cmd;
	s.vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	s.vkBeginCommandBuffer(cmd, &begin);

	// the previous frame, still running, is done with the buffers and
	// images this one writes again
	VkMemoryBarrier reuse{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reuse.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	reuse.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &reuse, 0, nullptr, 0, nullptr);

	// uploads
	VkBufferCopy region{ 0, 0, VkDeviceSize(total) * 4 };
	s.vkCmdCopyBuffer(cmd, sl.host.handle, s.frame_device.handle, 1, &region);
	if (upload_rom)
	{
		region.size = s.texture.size;
		s.vkCmdCopyBuffer(cmd, staging_texture.handle, s.texture.handle, 1, &region);
		region.size = s.sprite.size;
		s.vkCmdCopyBuffer(cmd, staging_sprite.handle, s.sprite.handle, 1, &region);
	}
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);

	push_constants push{};
	push.inv_width = 1.0f / s.width;
	push.inv_height = 1.0f / s.height;
	push.scale = s.scale;
	push.vertices = vertices;
	push.records = records;
	push.palette = pal;
	push.text = textw;
	push.textpri = priw;
	push.params = params;
	push.gamma = gamma;
	push.spot = spot;
	push.native_width = nwidth;
	VkRect2D const full{ { 0, 0 }, { u32(s.width), u32(s.height) } };
	VkViewport const viewport{ 0.0f, 0.0f, float(s.width), float(s.height), 0.0f, 1.0f };

	// the scene, over the background (and the text, on Super System 22)
	VkClearValue clears[2];
	clears[0].color = { { s.background.r() / 255.0f, s.background.g() / 255.0f, s.background.b() / 255.0f, 1.0f } };
	clears[1].color.uint32[0] = clears[1].color.uint32[1] = clears[1].color.uint32[2] = clears[1].color.uint32[3] = 0;
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = s.scene_pass;
	pass.framebuffer = s.scene_framebuffer;
	pass.renderArea = full;
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	s.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	s.vkCmdSetViewport(cmd, 0, 1, &viewport);
	s.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 2, s.sets, 0, nullptr);
	if (mix.super && mix.text_before)
	{
		push.pass = NP_TEXT_BEFORE;
		s.vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		s.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.before_pipeline);
		s.vkCmdSetScissor(cmd, 0, 1, &full);
		s.vkCmdDraw(cmd, 3, 1, 0, 0);
	}
	push.pass = NP_SCENE;
	s.vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.scene_pipeline);
	for (batch const &b : s.batches)
	{
		if (!b.scissor.extent.width || !b.scissor.extent.height)
			continue;
		s.vkCmdSetScissor(cmd, 0, 1, &b.scissor);
		s.vkCmdDraw(cmd, b.count, 1, b.first, 0);
	}
	s.vkCmdEndRenderPass(cmd);

	// text layer, fades and gamma into the display image
	pass.renderPass = s.mix_pass;
	pass.framebuffer = s.display_framebuffer;
	pass.clearValueCount = 0;
	pass.pClearValues = nullptr;
	s.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	s.vkCmdSetViewport(cmd, 0, 1, &viewport);
	s.vkCmdSetScissor(cmd, 0, 1, &full);
	push.pass = NP_FINAL;
	s.vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	s.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.final_pipeline);
	s.vkCmdDraw(cmd, 3, 1, 0, 0);
	s.vkCmdEndRenderPass(cmd);

	// into a texture of the display, or read back into the next entry of
	// the ring; screen pixel (x, y) at the high resolution is target pixel
	// (x - left, y - top)
	s32 const left = s.visible.left() * s.scale;
	s32 const top = s.visible.top() * s.scale;
	vk3d_share::target *const shared = s.share->acquire(rectangle(left, left + s.width - 1, top, top + s.height - 1));
	impl::readback_entry &entry = s.readback[s.next_readback];
	if (shared)
		s.share->record(cmd, *shared, s.display.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0);
	else
	{
		s.next_readback = (s.next_readback + 1) % READBACKS;
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { u32(s.width), u32(s.height), 1 };
		s.vkCmdCopyImageToBuffer(cmd, s.display.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.host.handle, 1, &copy);
		VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.buffer = entry.host.handle;
		host.size = VK_WHOLE_SIZE;
		s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	}
	s.vkEndCommandBuffer(cmd);

	// sent without waiting: frame_ready() waits when the renderer reads it
	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	s.vkResetFences(s.device, 1, &sl.fence);
	bool const ok = s.vkQueueSubmit(s.queue, 1, &submit, sl.fence) == VK_SUCCESS;
	if (ok && shared)
		s.share->submitted(*shared, true);
	sl.busy = ok;
	if (upload_rom)
	{
		// the staging buffers are freed once copied
		s.wait_slot(sl);
		s.destroy_buffer(staging_texture);
		s.destroy_buffer(staging_sprite);
		s.rom_uploaded = ok;
	}
	if (!ok)
		return nullptr;
	s.frames++;
	if (shared)
	{
		s.share->tag(*shared) = slot_index;
		return &s.share->bitmap(*shared);
	}
	entry.slot = slot_index;
	entry.ready = false;
	u32 *const base = reinterpret_cast<u32 *>(entry.host.mapped) - (ptrdiff_t(top) * s.width + left);
	entry.view.wrap(base, left + s.width, top + s.height, s.width);
	return &entry.view;
}


void namcos22_vk::frame_ready(bitmap_rgb32 const *frame)
{
	impl &s = *m;

	// a frame in a texture of the display: its batch must be done
	if (vk3d_share::target *const shared = s.share->find(frame))
	{
		int const slot = s.share->tag(*shared);
		if (slot >= 0)
			s.wait_slot(s.slots[slot]);
		return;
	}

	impl::readback_entry *found = nullptr;
	for (impl::readback_entry &entry : s.readback)
		if (&entry.view == frame)
			found = &entry;
	if (!found || found->ready || !found->host.handle)
		return;
	impl::readback_entry &entry = *found;
	if (entry.slot >= 0)
		s.wait_slot(s.slots[entry.slot]);
	if (!entry.host.coherent)
	{
		VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		range.memory = entry.host.memory;
		range.size = VK_WHOLE_SIZE;
		s.vkInvalidateMappedMemoryRanges(s.device, 1, &range);
	}
	entry.ready = true;
}


hires3d_output::gpu_texture namcos22_vk::shared_texture(bitmap_rgb32 const *frame)
{
	impl &s = *m;
	vk3d_share::target *const shared = s.share->find(frame);
	return shared ? s.share->texture(*shared) : hires3d_output::gpu_texture();
}
