// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    k001005_vk.cpp

    Vulkan renderer for the Konami K001005 at a higher resolution (mame3d).

    Triangles are recorded on the CPU in emulation order (a record per
    polygon, three vertices each) and sent to the GPU in batches, up to two
    in flight.  Each batch uploads its records, vertices and the palette,
    clears and draws into the page images, and may copy a finished page
    into a ring of read-back buffers that the display reads.

***************************************************************************/

#include "emu.h"
#include "k001005_vk.h"

#include "vk3d_context.h"
#include "vk3d_share.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

#include "k001005_vk_shaders/layout.h"

// shaders compiled with glslc from k001005_vk_shaders (see NOTES.md)
u32 const s_tri_vert[] = {
#include "k001005_vk_shaders/tri.vert.spv.inc"
};
u32 const s_tri_frag[] = {
#include "k001005_vk_shaders/tri.frag.spv.inc"
};
u32 const s_compose_vert[] = {
#include "k001005_vk_shaders/compose.vert.spv.inc"
};
u32 const s_compose_frag[] = {
#include "k001005_vk_shaders/compose.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr float FAR_DEPTH = 10000000000.0f / 1099511627776.0f; // cleared Z, scaled like the vertices
constexpr u32 PALETTE_SIZE = 0x800;
constexpr int SLOTS = 2;
constexpr int READBACKS = 3;

enum command_type : u8 { CMD_TRIANGLES, CMD_CLEAR };

struct command
{
	command_type type;
	u8 page;
	bool zcompare;
	VkRect2D scissor;
	u32 first;          // first vertex
	u32 count;
};

struct push_constants
{
	float inv_width, inv_height;
	u32 vertices, palette;
};

struct compose_constants
{
	u32 back, front;
	s32 native_width, scale, with_3d;
};

} // anonymous namespace


struct k001005_vk::impl : vk3d_context
{
	struct slot
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool busy = false;
		buffer host;
	};

	struct readback_entry
	{
		buffer host;
		bitmap_rgb32 view;      // over host, for the display (composed frames)
		int page = -1;          // page it holds, -1 if none, -2 for a composed frame
		u32 version = 0;        // version of that page
		int slot = -1;          // batch that copies it
		bool ready = false;
	};

	VkRenderPass passes[4]{};   // colour cleared (1) or loaded, depth cleared (2) or loaded
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipeline pipelines[2]{};  // without, with the Z compare

	// composing the screen into the display image
	VkRenderPass compose_pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout compose_set_layout = VK_NULL_HANDLE;
	VkPipelineLayout compose_layout = VK_NULL_HANDLE;
	VkDescriptorPool compose_pool = VK_NULL_HANDLE;
	VkDescriptorSet compose_sets[2]{};
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipeline compose_pipeline = VK_NULL_HANDLE;
	image display;
	VkFramebuffer display_framebuffer = VK_NULL_HANDLE;
	bool compose_mode = false;  // the pages are no longer read back on their own
	bool compose_pending = false;
	int compose_page = 0;
	bool compose_3d = false;
	int native_width = 0;
	std::vector<u32> layers;    // native back and front layers
	int shown_readback = -1;

	// textures of the display the composed frames go to instead of being read back
	std::unique_ptr<vk3d_share> share;
	vk3d_share::target *compose_shared = nullptr; // texture of the frame being composed
	vk3d_share::target *shown_shared = nullptr;   // texture of the frame shown
	rectangle last_clip;        // area and page version of the last composed frame
	u32 composed_version = 0;

	int width = 0;
	int height = 0;
	image color[2];
	image depth;
	VkFramebuffer framebuffers[2]{};
	bool color_ready[2]{};
	bool depth_ready = false;

	pen_t const *palette = nullptr;
	std::vector<u8> texture_data;
	buffer texture;
	bool texture_uploaded = false;
	buffer data;
	bool descriptors_dirty = true;
	slot slots[SLOTS];
	int next_slot = 0;

	readback_entry readback[READBACKS];
	int next_readback = 0;
	int last_readback = -1;
	u32 versions[2]{};

	std::vector<u32> records;
	std::vector<u32> vertices;
	std::vector<command> commands;

	// statistics, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u64 triangles = 0;
	u32 flushes = 0;
	u32 composes = 0;
	u32 swaps = 0;

	~impl();

	bool init(std::string &error);
	bool init_compose(std::string &error);
	VkPipeline make_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool zcompare);
	bool create_targets();
	void wait_slot(slot &target);
	void wait_all();
	void add(command const &cmd);
	void begin_pass(VkCommandBuffer cmd, int page);
	bool flush(int readback_page, bool wait);
	void complete(readback_entry &entry);
	int current_readback(int page);
};


//**************************************************************************
//  SETUP
//**************************************************************************

k001005_vk::impl::~impl()
{
	if (flushes)
		osd_printf_verbose("k001005_vk: %u batches, %u swaps, %u composed frames, %u triangles, %.1f ms waiting for the GPU\n",
				flushes, swaps, composes, u32(triangles), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		for (int i = 0; i < 2; i++)
		{
			if (framebuffers[i])
				vkDestroyFramebuffer(device, framebuffers[i], nullptr);
			destroy_image(color[i]);
		}
		destroy_image(depth);
		if (display_framebuffer)
			vkDestroyFramebuffer(device, display_framebuffer, nullptr);
		destroy_image(display);
		for (readback_entry &entry : readback)
			destroy_buffer(entry.host);
		if (compose_pipeline)
			vkDestroyPipeline(device, compose_pipeline, nullptr);
		if (compose_layout)
			vkDestroyPipelineLayout(device, compose_layout, nullptr);
		if (compose_pool)
			vkDestroyDescriptorPool(device, compose_pool, nullptr);
		if (compose_set_layout)
			vkDestroyDescriptorSetLayout(device, compose_set_layout, nullptr);
		if (sampler)
			vkDestroySampler(device, sampler, nullptr);
		if (compose_pass)
			vkDestroyRenderPass(device, compose_pass, nullptr);
		for (slot &target : slots)
		{
			destroy_buffer(target.host);
			if (target.fence)
				vkDestroyFence(device, target.fence, nullptr);
		}
		destroy_buffer(texture);
		destroy_buffer(data);
		for (VkPipeline p : pipelines)
			if (p)
				vkDestroyPipeline(device, p, nullptr);
		if (layout)
			vkDestroyPipelineLayout(device, layout, nullptr);
		if (pool)
			vkDestroyDescriptorPool(device, pool, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		for (VkRenderPass pass : passes)
			if (pass)
				vkDestroyRenderPass(device, pass, nullptr);
	}
}


bool k001005_vk::impl::init(std::string &error)
{
	// dual-source blending for the frame buffer blends, depth clamping for
	// the polygons behind the camera (the chip does not clip them in Z)
	VkPhysicalDeviceFeatures wanted{};
	wanted.dualSrcBlend = VK_TRUE;
	wanted.depthClamp = VK_TRUE;
	if (!init_device(error, &wanted))
		return false;
	share = std::make_unique<vk3d_share>(*this);
	if (!enabled_features().dualSrcBlend || !enabled_features().depthClamp)
	{
		error = "the GPU lacks dualSrcBlend or depthClamp";
		return false;
	}

	// render passes: images stay in attachment layout between passes
	for (int variant = 0; variant < 4; variant++)
	{
		bool const clear_color = (variant & 1) != 0;
		bool const clear_depth = (variant & 2) != 0;
		VkAttachmentDescription attachments[2]{};
		attachments[0].format = COLOR_FORMAT;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = clear_color ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachments[1].format = DEPTH_FORMAT;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = clear_depth ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkAttachmentReference depth_ref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color_ref;
		subpass.pDepthStencilAttachment = &depth_ref;
		VkPipelineStageFlags const stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkAccessFlags const access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[0].dstStageMask = stages;
		dependencies[0].srcAccessMask = access | VK_ACCESS_TRANSFER_READ_BIT;
		dependencies[0].dstAccessMask = access;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = stages;
		dependencies[1].dstStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[1].srcAccessMask = access;
		dependencies[1].dstAccessMask = access | VK_ACCESS_TRANSFER_READ_BIT;
		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 2;
		info.pAttachments = attachments;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = 2;
		info.pDependencies = dependencies;
		if (vkCreateRenderPass(device, &info, nullptr, &passes[variant]) != VK_SUCCESS)
		{
			error = "vkCreateRenderPass failed";
			return false;
		}
	}

	VkDescriptorSetLayoutBinding bindings[2]{};
	for (int i = 0; i < 2; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 2;
	set_info.pBindings = bindings;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
	VkPushConstantRange push_range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants) };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
	VkDescriptorPoolSize pool_size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
	VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
	VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorPool = pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &set_layout;
	if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS)
	{
		error = "descriptor set allocation failed";
		return false;
	}

	VkShaderModule const vertex = create_module(s_tri_vert, sizeof(s_tri_vert));
	VkShaderModule const fragment = create_module(s_tri_frag, sizeof(s_tri_frag));
	if (!vertex || !fragment)
	{
		error = "shader rejected";
		return false;
	}
	pipelines[0] = make_pipeline(vertex, fragment, false);
	pipelines[1] = make_pipeline(vertex, fragment, true);
	if (!pipelines[0] || !pipelines[1])
	{
		error = "pipeline creation failed";
		return false;
	}
	if (!init_compose(error))
		return false;

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


bool k001005_vk::impl::init_compose(std::string &error)
{
	// render pass into the display image, left ready for the copy
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
	VkRenderPassCreateInfo pass_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	pass_info.attachmentCount = 1;
	pass_info.pAttachments = &attachment;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	pass_info.dependencyCount = 2;
	pass_info.pDependencies = dependencies;
	if (vkCreateRenderPass(device, &pass_info, nullptr, &compose_pass) != VK_SUCCESS)
	{
		error = "vkCreateRenderPass failed";
		return false;
	}

	// set 0: the frame data, set 1: a page
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 1;
	set_info.pBindings = &binding;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &compose_set_layout);
	VkDescriptorSetLayout const set_layouts[2] = { set_layout, compose_set_layout };
	VkPushConstantRange push_range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(compose_constants) };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 2;
	layout_info.pSetLayouts = set_layouts;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &compose_layout);
	VkDescriptorPoolSize pool_size{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
	VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pool_info.maxSets = 2;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	vkCreateDescriptorPool(device, &pool_info, nullptr, &compose_pool);
	VkDescriptorSetLayout const page_layouts[2] = { compose_set_layout, compose_set_layout };
	VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorPool = compose_pool;
	alloc.descriptorSetCount = 2;
	alloc.pSetLayouts = page_layouts;
	if (vkAllocateDescriptorSets(device, &alloc, compose_sets) != VK_SUCCESS)
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

	VkShaderModule const vertex = create_module(s_compose_vert, sizeof(s_compose_vert));
	VkShaderModule const fragment = create_module(s_compose_frag, sizeof(s_compose_frag));
	if (!vertex || !fragment)
	{
		error = "shader rejected";
		return false;
	}
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
	VkPipelineColorBlendAttachmentState blend{};
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = compose_layout;
	info.renderPass = compose_pass;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &compose_pipeline) != VK_SUCCESS)
	{
		error = "compose pipeline creation failed";
		return false;
	}
	return true;
}


VkPipeline k001005_vk::impl::make_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool zcompare)
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
	raster.depthClampEnable = VK_TRUE;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Z <= stored Z when comparing, always written
	VkPipelineDepthStencilStateCreateInfo depth_state{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth_state.depthTestEnable = VK_TRUE;
	depth_state.depthWriteEnable = VK_TRUE;
	depth_state.depthCompareOp = zcompare ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

	// colour = A + K * destination, A in output 0, 1 - K in output 1
	VkPipelineColorBlendAttachmentState blend{};
	blend.blendEnable = VK_TRUE;
	blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
	blend.colorBlendOp = VK_BLEND_OP_ADD;
	blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
	blend.alphaBlendOp = VK_BLEND_OP_ADD;
	blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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
	info.pDepthStencilState = &depth_state;
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = layout;
	info.renderPass = passes[0];
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return result;
}


bool k001005_vk::impl::create_targets()
{
	for (int i = 0; i < 2; i++)
		if (!create_image(color[i], width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
			return false;
	if (!create_image(display, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
		return false;
	VkFramebufferCreateInfo display_info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	display_info.renderPass = compose_pass;
	display_info.attachmentCount = 1;
	display_info.pAttachments = &display.view;
	display_info.width = width;
	display_info.height = height;
	display_info.layers = 1;
	if (vkCreateFramebuffer(device, &display_info, nullptr, &display_framebuffer) != VK_SUCCESS)
		return false;
	for (int i = 0; i < 2; i++)
	{
		VkDescriptorImageInfo image_info{ sampler, color[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = compose_sets[i];
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &image_info;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	}
	if (!create_image(depth, width, height, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
		return false;
	for (int i = 0; i < 2; i++)
	{
		VkImageView const views[2] = { color[i].view, depth.view };
		VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		info.renderPass = passes[0];
		info.attachmentCount = 2;
		info.pAttachments = views;
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (vkCreateFramebuffer(device, &info, nullptr, &framebuffers[i]) != VK_SUCCESS)
			return false;
	}
	for (readback_entry &entry : readback)
		if (!create_buffer(entry.host, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			return false;
	return true;
}


void k001005_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void k001005_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


void k001005_vk::impl::add(command const &cmd)
{
	versions[cmd.page]++;
	if (!commands.empty() && (cmd.type == CMD_TRIANGLES))
	{
		command &last = commands.back();
		if ((last.type == CMD_TRIANGLES) && (last.page == cmd.page) && (last.zcompare == cmd.zcompare) &&
				(last.first + last.count == cmd.first) && !std::memcmp(&last.scissor, &cmd.scissor, sizeof(VkRect2D)))
		{
			last.count += cmd.count;
			return;
		}
	}
	commands.push_back(cmd);

	// send batches to the GPU as they fill, without waiting
	if (vertices.size() >= 3 * 1024 * KV_SIZE)
		flush(-1, false);
}


void k001005_vk::impl::begin_pass(VkCommandBuffer cmd, int page)
{
	int const variant = (color_ready[page] ? 0 : 1) | (depth_ready ? 0 : 2);
	VkClearValue clears[2];
	clears[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
	clears[1].depthStencil = { FAR_DEPTH, 0 };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = passes[variant];
	pass.framebuffer = framebuffers[page];
	pass.renderArea = { { 0, 0 }, { u32(width), u32(height) } };
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	color_ready[page] = true;
	depth_ready = true;
	VkViewport const viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
}


//-------------------------------------------------
//  flush - send the pending work to the GPU, and
//  copy a page to a read-back buffer if asked
//-------------------------------------------------

bool k001005_vk::impl::flush(int readback_page, bool wait)
{
	if (commands.empty() && (readback_page < 0) && !compose_pending)
		return true;

	int const slot_index = next_slot;
	slot &sl = slots[slot_index];
	next_slot = (next_slot + 1) % SLOTS;
	wait_slot(sl);

	// palette, records, vertices
	u32 const pal = 0;
	u32 const rec = PALETTE_SIZE;
	u32 const vert = rec + u32(records.size());
	u32 const lay = vert + u32(vertices.size());
	u32 const total = lay + (compose_pending ? u32(layers.size()) : 0);
	VkDeviceSize const bytes = VkDeviceSize(total) * 4;
	if (data.size < bytes)
	{
		wait_all();
		destroy_buffer(data);
		if (!create_device_buffer(data, std::max<VkDeviceSize>(bytes * 3 / 2, 1 << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
			return false;
		descriptors_dirty = true;
	}
	if (sl.host.size < bytes)
	{
		destroy_buffer(sl.host);
		if (!create_buffer(sl.host, std::max<VkDeviceSize>(bytes * 3 / 2, 1 << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
			return false;
	}
	u32 *const out = reinterpret_cast<u32 *>(sl.host.mapped);
	for (u32 i = 0; i < PALETTE_SIZE; i++)
		out[pal + i] = palette ? u32(palette[i]) : 0;
	std::copy(records.begin(), records.end(), out + rec);
	std::copy(vertices.begin(), vertices.end(), out + vert);
	if (compose_pending)
		std::copy(layers.begin(), layers.end(), out + lay);
	flush_buffer(sl.host, 0, bytes);

	// the texture ROM, once
	buffer staging;
	bool const upload_texture = !texture_uploaded;
	if (upload_texture)
	{
		wait_all();
		VkDeviceSize const texture_bytes = std::max<size_t>((texture_data.size() + 3) & ~size_t(3), 4);
		destroy_buffer(texture);
		if (!create_device_buffer(texture, texture_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
				!create_buffer(staging, texture_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
			return false;
		std::memset(staging.mapped, 0, texture_bytes);
		std::memcpy(staging.mapped, texture_data.data(), texture_data.size());
		flush_buffer(staging, 0, texture_bytes);
		descriptors_dirty = true;
	}
	if (descriptors_dirty)
	{
		wait_all();
		buffer const *const sources[2] = { &texture, &data };
		VkDescriptorBufferInfo infos[2];
		VkWriteDescriptorSet writes[2]{};
		for (int i = 0; i < 2; i++)
		{
			infos[i] = { sources[i]->handle, 0, VK_WHOLE_SIZE };
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}
		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		descriptors_dirty = false;
	}

	VkCommandBuffer const cmd = sl.cmd;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	// uploads, once the previous batch is done reading
	VkMemoryBarrier reused{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reused.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	reused.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reused, 0, nullptr, 0, nullptr);
	VkBufferCopy region{ 0, 0, bytes };
	vkCmdCopyBuffer(cmd, sl.host.handle, data.handle, 1, &region);
	if (upload_texture)
	{
		region.size = texture.size;
		vkCmdCopyBuffer(cmd, staging.handle, texture.handle, 1, &region);
	}
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);

	// replay
	push_constants const push = { 1.0f / width, 1.0f / height, vert, pal };
	int current = -1;
	int bound = -1;
	for (command const &c : commands)
	{
		if (c.page != current)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			begin_pass(cmd, c.page);
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			current = c.page;
			bound = -1;
		}
		if (c.type == CMD_CLEAR)
		{
			VkClearAttachment attachments[2] = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, {} }, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, {} } };
			attachments[0].clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
			attachments[1].clearValue.depthStencil = { FAR_DEPTH, 0 };
			VkClearRect const rect{ { { 0, 0 }, { u32(width), u32(height) } }, 0, 1 };
			vkCmdClearAttachments(cmd, 2, attachments, 1, &rect);
			continue;
		}
		if (!c.scissor.extent.width || !c.scissor.extent.height)
			continue;
		if (int(c.zcompare) != bound)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[c.zcompare ? 1 : 0]);
			bound = c.zcompare;
		}
		vkCmdSetScissor(cmd, 0, 1, &c.scissor);
		vkCmdDraw(cmd, c.count, 1, c.first, 0);
	}
	if (current >= 0)
		vkCmdEndRenderPass(cmd);

	// compose the screen into the display image and copy it out
	int rb = -1;
	if (compose_pending)
	{
		int const page = compose_page;
		if (!color_ready[page])
		{
			begin_pass(cmd, page);
			vkCmdEndRenderPass(cmd);
		}
		if (!compose_shared)
		{
			for (int i = 0; (i < READBACKS) && (rb < 0); i++)
				if (((next_readback + i) % READBACKS) != shown_readback)
					rb = (next_readback + i) % READBACKS;
			next_readback = (rb + 1) % READBACKS;
		}

		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = color[page].handle;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		pass.renderPass = compose_pass;
		pass.framebuffer = display_framebuffer;
		pass.renderArea = { { 0, 0 }, { u32(width), u32(height) } };
		vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
		VkViewport const viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
		vkCmdSetViewport(cmd, 0, 1, &viewport);
		vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_pipeline);
		VkDescriptorSet const sets[2] = { set, compose_sets[page] };
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, compose_layout, 0, 2, sets, 0, nullptr);
		compose_constants const constants = { lay, lay + u32(layers.size() / 2), native_width, width / native_width, compose_3d ? 1 : 0 };
		vkCmdPushConstants(cmd, compose_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRenderPass(cmd);
		barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

		if (compose_shared)
			share->record(cmd, *compose_shared, display.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0);
		else
		{
			VkBufferImageCopy copy{};
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.imageExtent = { u32(width), u32(height), 1 };
			vkCmdCopyImageToBuffer(cmd, display.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback[rb].host.handle, 1, &copy);
			VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			host.buffer = readback[rb].host.handle;
			host.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
		}
		readback_page = -1;
	}

	// copy a page out
	if (readback_page >= 0)
	{
		if (!color_ready[readback_page])
		{
			begin_pass(cmd, readback_page);
			vkCmdEndRenderPass(cmd);
		}
		rb = next_readback;
		next_readback = (next_readback + 1) % READBACKS;
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = color[readback_page].handle;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { u32(width), u32(height), 1 };
		vkCmdCopyImageToBuffer(cmd, color[readback_page].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback[rb].host.handle, 1, &copy);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		host.buffer = readback[rb].host.handle;
		host.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	}
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vkResetFences(device, 1, &sl.fence);
	bool const ok = vkQueueSubmit(queue, 1, &submit, sl.fence) == VK_SUCCESS;
	sl.busy = ok;
	flushes++;
	if (ok && compose_pending && compose_shared)
	{
		share->submitted(*compose_shared, true);
		share->tag(*compose_shared) = slot_index;
	}
	commands.clear();
	records.clear();
	vertices.clear();
	if (upload_texture)
	{
		// the staging buffer must outlive the copy
		wait_slot(sl);
		destroy_buffer(staging);
		texture_uploaded = ok;
	}
	if (!ok)
		return false;

	if (rb >= 0 && compose_pending)
	{
		readback[rb].page = -2;
		readback[rb].slot = slot_index;
		readback[rb].ready = false;
		last_readback = rb;
	}
	else if (rb >= 0)
	{
		readback[rb].page = readback_page;
		readback[rb].version = versions[readback_page];
		readback[rb].slot = slot_index;
		readback[rb].ready = false;
		last_readback = rb;
	}
	if (wait)
		wait_slot(sl);
	return true;
}


void k001005_vk::impl::complete(readback_entry &entry)
{
	if (entry.ready)
		return;
	if (entry.slot >= 0)
		wait_slot(slots[entry.slot]);
	if (!entry.host.coherent)
	{
		VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		range.memory = entry.host.memory;
		range.size = VK_WHOLE_SIZE;
		vkInvalidateMappedMemoryRanges(device, 1, &range);
	}
	entry.ready = true;
}


int k001005_vk::impl::current_readback(int page)
{
	int found = -1;
	for (int i = 0; i < READBACKS; i++)
		if ((readback[i].page == page) && (readback[i].version == versions[page]))
			found = i;
	if (found < 0)
	{
		if (!flush(page, false) || (last_readback < 0))
			return -1;
		found = last_readback;
	}
	complete(readback[found]);
	return found;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

std::unique_ptr<k001005_vk> k001005_vk::create(int width, int height, std::string &error)
{
	auto state = std::make_unique<impl>();
	if (!state->init(error))
		return nullptr;
	state->width = width;
	state->height = height;
	if (!state->create_targets())
	{
		error = "frame buffer allocation failed";
		return nullptr;
	}
	return std::unique_ptr<k001005_vk>(new k001005_vk(std::move(state)));
}


k001005_vk::k001005_vk(std::unique_ptr<impl> &&state)
	: m(std::move(state))
{
}


k001005_vk::~k001005_vk()
{
}


std::string const &k001005_vk::device_name() const
{
	return m->name;
}


void k001005_vk::set_texture_memory(u8 const *data, size_t bytes)
{
	m->texture_data.assign(data, data + bytes);
	m->texture_uploaded = false;
}


void k001005_vk::set_palette(pen_t const *pens)
{
	m->palette = pens;
}


void k001005_vk::triangle(int page, poly const &p, vertex const *v, rectangle const &clip)
{
	impl &s = *m;
	u32 const record = PALETTE_SIZE + u32(s.records.size());
	u32 words[KR_SIZE]{};
	words[KR_TEXX] = p.texture_x;
	words[KR_TEXY] = p.texture_y;
	words[KR_TEXW] = p.texture_width;
	words[KR_TEXH] = p.texture_height;
	words[KR_PAGE] = p.texture_page * 0x40000;
	words[KR_PAL] = p.texture_palette * 256;
	words[KR_FLAGS] = (p.mirror ? KF_MIRROR : 0) | (p.fog ? KF_FOG : 0) | (p.bilinear ? KF_BILINEAR : 0) |
			(p.texture ? KF_TEXTURE : 0) | (p.vertexcolor ? KF_VERTEXCOLOR : 0) | (p.fbblend ? KF_FBBLEND : 0);
	words[KR_DIFFUSE] = p.diffuse;
	words[KR_AMBIENT] = p.ambient;
	words[KR_FOGCOLOR] = p.fogcolor;
	words[KR_POLYCOLOR] = p.polycolor;
	s.records.insert(s.records.end(), std::begin(words), std::end(words));

	u32 const first = u32(s.vertices.size() / KV_SIZE);
	for (int i = 0; i < 3; i++)
	{
		float const values[12] = { v[i].x, v[i].y, v[i].z, v[i].fog, v[i].diff, v[i].u, v[i].v, v[i].w, v[i].r, v[i].g, v[i].b, v[i].a };
		for (float value : values)
		{
			u32 bits;
			std::memcpy(&bits, &value, 4);
			s.vertices.push_back(bits);
		}
		s.vertices.push_back(record);
		s.vertices.insert(s.vertices.end(), KV_SIZE - 13, 0);
	}

	s32 const left = std::clamp<s32>(clip.left(), 0, s.width);
	s32 const right = std::clamp<s32>(clip.right() + 1, left, s.width);
	s32 const top = std::clamp<s32>(clip.top(), 0, s.height);
	s32 const bottom = std::clamp<s32>(clip.bottom() + 1, top, s.height);
	command cmd{};
	cmd.type = CMD_TRIANGLES;
	cmd.page = u8(page & 1);
	cmd.zcompare = p.zcompare;
	cmd.scissor = { { left, top }, { u32(right - left), u32(bottom - top) } };
	cmd.first = first;
	cmd.count = 3;
	s.triangles++;
	s.add(cmd);
}


void k001005_vk::clear(int page)
{
	command cmd{};
	cmd.type = CMD_CLEAR;
	cmd.page = u8(page & 1);
	m->add(cmd);
}


void k001005_vk::prefetch(int page)
{
	impl &s = *m;
	s.swaps++;
	if (s.compose_mode)
	{
		// the page is composed later: just start drawing it on the GPU
		s.flush(-1, false);
		return;
	}
	page &= 1;
	for (impl::readback_entry const &entry : s.readback)
		if ((entry.page == page) && (entry.version == s.versions[page]))
			return;
	s.flush(page, false);
}


bool k001005_vk::draw(int page, bitmap_rgb32 &dest, rectangle const &clip, int step)
{
	impl &s = *m;
	int const index = s.current_readback(page & 1);
	if (index < 0)
		return false;

	// the frame buffer pixel (0, 0) goes to the top left of clip, like the
	// software renderer
	u32 const *const image = reinterpret_cast<u32 const *>(s.readback[index].host.mapped);
	for (int y = clip.top(); y <= clip.bottom(); y++)
	{
		int const sy = (y - clip.top()) * step;
		if (sy >= s.height)
			break;
		u32 const *const src = image + size_t(sy) * s.width;
		u32 *const dst = &dest.pix(y);
		for (int x = clip.left(); x <= clip.right(); x++)
		{
			int const sx = (x - clip.left()) * step;
			if (sx >= s.width)
				break;
			if (src[sx] & 0xff000000)
				dst[x] = src[sx];
		}
	}
	return true;
}


bitmap_rgb32 *k001005_vk::compose(int page, bool with_3d, bitmap_rgb32 const &back, bitmap_rgb32 const &front, rectangle const &clip, int scale)
{
	impl &s = *m;
	if ((clip.width() * scale != s.width) || (clip.height() * scale != s.height))
		return nullptr;

	// the native layers of the area the page covers; when they, the page and
	// the area are unchanged, the last composed frame is shown again
	int const nwidth = clip.width();
	int const nheight = clip.height();
	page &= 1;
	bool const composed = s.shown_shared || ((s.shown_readback >= 0) && (s.readback[s.shown_readback].page == -2));
	bool same = composed &&
			(s.layers.size() == size_t(2) * nwidth * nheight) && (s.last_clip == clip) &&
			(s.compose_page == page) && (s.compose_3d == with_3d) && (s.composed_version == s.versions[page]);
	s.compose_mode = true;
	s.native_width = nwidth;
	s.layers.resize(size_t(2) * nwidth * nheight);
	for (int y = 0; y < nheight; y++)
	{
		u32 *const backrow = &s.layers[size_t(y) * nwidth];
		u32 *const frontrow = &s.layers[size_t(nheight + y) * nwidth];
		u32 const *const backsrc = &back.pix(clip.top() + y, clip.left());
		u32 const *const frontsrc = &front.pix(clip.top() + y, clip.left());
		if (same && (std::memcmp(backrow, backsrc, nwidth * 4) || std::memcmp(frontrow, frontsrc, nwidth * 4)))
			same = false;
		if (!same)
		{
			std::memcpy(backrow, backsrc, nwidth * 4);
			std::memcpy(frontrow, frontsrc, nwidth * 4);
		}
	}
	if (same)
		return s.shown_shared ? &s.share->bitmap(*s.shown_shared) : &s.readback[s.shown_readback].view;
	s.last_clip = clip;
	s.composed_version = s.versions[page];
	// the batch holds the composition only: the triangles of the page being
	// drawn stay for the next one, so that it does not wait for them
	std::vector<command> commands;
	std::vector<u32> records, vertices;
	commands.swap(s.commands);
	records.swap(s.records);
	vertices.swap(s.vertices);
	s.composes++;
	s.compose_pending = true;
	s.compose_page = page & 1;
	s.compose_3d = with_3d;

	// into a texture of the display when it can draw one: high-resolution
	// screen pixel (x, y) is image pixel (x - left, y - top)
	s32 const left = clip.left() * scale;
	s32 const top = clip.top() * scale;
	s.compose_shared = s.share->acquire(rectangle(left, left + s.width - 1, top, top + s.height - 1));
	vk3d_share::target *const shared = s.compose_shared;
	bool const ok = s.flush(-1, true);
	s.compose_pending = false;
	s.compose_shared = nullptr;
	s.commands.swap(commands);
	s.records.swap(records);
	s.vertices.swap(vertices);
	if (ok && shared)
	{
		s.shown_shared = shared;
		s.shown_readback = -1;
		return &s.share->bitmap(*shared);
	}
	s.shown_shared = nullptr;
	if (!ok || (s.last_readback < 0))
		return nullptr;
	int const index = s.last_readback;
	impl::readback_entry &entry = s.readback[index];
	s.complete(entry);

	u32 *const base = reinterpret_cast<u32 *>(entry.host.mapped) - (ptrdiff_t(top) * s.width + left);
	entry.view.wrap(base, left + s.width, top + s.height, s.width);
	s.shown_readback = index;
	return &entry.view;
}


hires3d_output::gpu_texture k001005_vk::shared_texture(bitmap_rgb32 const *frame)
{
	impl &s = *m;
	vk3d_share::target *const shared = s.share->find(frame);
	return shared ? s.share->texture(*shared) : hires3d_output::gpu_texture();
}
