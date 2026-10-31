// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    voodoo_vk.cpp

    Vulkan renderer for the Voodoo 1/2 at a higher resolution (mame3d).

    Work is recorded on the CPU in emulation order (triangle records, CPU
    pixel writes, clears) and sent to the GPU in batches: every few
    thousand triangles without waiting, and when the display needs the
    front buffer, which waits for the image.  Each batch uploads its
    records, the new palettes and fog tables and the texture memory pages
    that changed, then replays the commands into the colour images,
    switching render passes when the target buffer changes.

***************************************************************************/

#include "emu.h"
#include "voodoo.h"

#include "vk3d_context.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>


namespace voodoo {

namespace {

#include "voodoo_vk_shaders/layout.h"

// shaders compiled with glslc from voodoo_vk_shaders (see NOTES.md)
u32 const s_tri_vert[] = {
#include "voodoo_vk_shaders/tri.vert.spv.inc"
};
u32 const s_tri_frag[] = {
#include "voodoo_vk_shaders/tri.frag.spv.inc"
};
u32 const s_lfb_vert[] = {
#include "voodoo_vk_shaders/lfb.vert.spv.inc"
};
u32 const s_lfb_frag[] = {
#include "voodoo_vk_shaders/lfb.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

// texture memory is uploaded in pages
constexpr u32 PAGE_SHIFT = 12;

// pipeline keys: triangles use the low bits (see triangle_key), the CPU
// pixel writes have their own
constexpr u32 KEY_BLEND = 1 << 0;           // + source factor << 1, destination factor << 5
constexpr u32 KEY_SRC_ALPHA_ONE = 1 << 9;
constexpr u32 KEY_DST_ALPHA_ONE = 1 << 10;
constexpr u32 KEY_DEPTH = 1 << 11;          // + depth function << 12
constexpr u32 KEY_DEPTH_WRITE = 1 << 15;
constexpr u32 KEY_RGB_WRITE = 1 << 16;
constexpr u32 KEY_ALPHA_WRITE = 1 << 17;
constexpr u32 KEY_LFB_COLOR = 0x80000000;
constexpr u32 KEY_LFB_DEPTH = 0x80000001;

enum command_type : u8 { CMD_TRIANGLES, CMD_CLEAR, CMD_LFB_COLOR, CMD_LFB_DEPTH };

struct command
{
	command_type type;
	s8 buffer;
	bool clear_color;
	bool clear_depth;
	u32 key;
	VkRect2D rect;      // scissor, or area to clear
	u32 first;          // first triangle or pixel
	u32 count;
	u32 color;          // clear colour
	float depth;        // clear depth
};

inline u32 float_bits(float value)
{
	u32 bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

inline void put64(u32 *dest, s64 value)
{
	dest[0] = u32(value);
	dest[1] = u32(u64(value) >> 32);
}

VkBlendFactor src_factor(u32 code)
{
	switch (code)
	{
		case 1: return VK_BLEND_FACTOR_SRC_ALPHA;
		case 2: return VK_BLEND_FACTOR_DST_COLOR;
		case 3: return VK_BLEND_FACTOR_DST_ALPHA;
		case 4: return VK_BLEND_FACTOR_ONE;
		case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case 6: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case 15: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		default: return VK_BLEND_FACTOR_ZERO;
	}
}

VkBlendFactor dst_factor(u32 code)
{
	switch (code)
	{
		case 1: return VK_BLEND_FACTOR_SRC_ALPHA;
		case 2: return VK_BLEND_FACTOR_SRC_COLOR;
		case 3: return VK_BLEND_FACTOR_DST_ALPHA;
		case 4: return VK_BLEND_FACTOR_ONE;
		case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case 15: return VK_BLEND_FACTOR_SRC1_COLOR; // colour before fog
		default: return VK_BLEND_FACTOR_ZERO;
	}
}

} // anonymous namespace


//**************************************************************************
//  STATE
//**************************************************************************

struct voodoo_vk::impl : vk3d_context
{
	// a host buffer copied into a device buffer before drawing
	struct stream
	{
		buffer host;
		buffer device;
	};

	struct palette
	{
		rgb_t const *source;
		u32 index;
	};

	int scale = 1;

	VkRenderPass passes[4]{};   // colour cleared (1) or loaded, depth cleared (2) or loaded
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkShaderModule tri_vert = VK_NULL_HANDLE;
	VkShaderModule tri_frag = VK_NULL_HANDLE;
	VkShaderModule lfb_vert = VK_NULL_HANDLE;
	VkShaderModule lfb_frag = VK_NULL_HANDLE;
	std::unordered_map<u32, VkPipeline> pipelines;

	// targets
	u32 rowpixels = 0;
	u32 rows = 0;
	int width = 0;
	int height = 0;
	image color[3];
	image depth;
	VkFramebuffer framebuffers[3]{};
	bool color_ready[3]{};
	bool depth_ready = false;
	buffer readback;
	int readback_buffer = -1;   // buffer whose current image is in readback

	// batch data: each batch copies its records, offsets, palettes and fog
	// tables, and pixels from its own host buffers into the device buffers;
	// up to SLOTS batches are in flight
	enum { DATA_RECORDS, DATA_OFFSETS, DATA_LOOKUP, DATA_PIXELS, DATA_COUNT };
	static constexpr int SLOTS = 3;
	struct slot
	{
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool busy = false;
		buffer host[DATA_COUNT];
	};
	slot slots[SLOTS];
	int next_slot = 0;
	buffer data[DATA_COUNT];
	stream tmu[2];              // host mirror shared by the batches
	bool descriptors_dirty = true;

	// texture memory
	u8 const *tmu_ram[2]{};
	u32 tmu_size[2]{};
	std::vector<u8> tmu_dirty[2];
	bool tmu_any_dirty[2]{};

	// texel tables, palettes and fog tables; the static tables come first
	rgb_t const *static_tables[7]{};
	std::vector<u32> lookup_data;
	bool static_uploaded = false;
	std::vector<palette> palettes;
	rgb_t const *last_texture[2]{};        // last palette of each TMU
	u32 last_texture_lookup[2]{};
	u8 fog_cache[128]{};
	u32 fog_index = ~0U;

	// pending work
	std::vector<u32> record_data;
	std::vector<u32> offset_data;
	std::vector<u32> pixel_data;
	std::vector<command> commands;
	bool textured_pending = false;

	// time spent waiting for the GPU, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u32 flushes = 0;
	u32 readbacks = 0;
	u64 texture_bytes = 0;

	~impl();

	bool init(std::string &error);
	VkPipeline pipeline(u32 key);
	bool create_targets();
	void destroy_targets();
	bool ensure_stream(stream &s, VkDeviceSize bytes);
	bool ensure_host(buffer &buf, VkDeviceSize bytes);
	bool ensure_device(buffer &buf, VkDeviceSize bytes);
	void wait_slot(slot &target);
	void wait_all();
	void reset_batch();
	u32 lookup_index(int tmu, rasterizer_texture const &tex);
	u32 fog_table(u8 const *blend, u8 const *delta);
	void texture_block(u32 *block, int tmu, rasterizer_texture const &tex, u32 texmode, s64 const *iter);
	void add(command const &cmd);
	VkRect2D clip(s32 left, s32 top, s32 right, s32 bottom) const; // right/bottom exclusive
	void begin_pass(VkCommandBuffer cmd, int buffer);
	bool flush(int readback_index, bool wait);
};


voodoo_vk::impl::~impl()
{
	if (flushes)
		osd_printf_verbose("voodoo_vk: %dx%d images, %u batches, %u read-backs, %.1f ms waiting for the GPU (%.2f ms per batch), %.1f MB of textures uploaded\n",
				width, height, flushes, readbacks, wait_ticks * 1000.0 / osd_ticks_per_second(), wait_ticks * 1000.0 / osd_ticks_per_second() / flushes, texture_bytes / 1048576.0);
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		destroy_targets();
		for (slot &target : slots)
		{
			for (buffer &buf : target.host)
				destroy_buffer(buf);
			if (target.fence)
				vkDestroyFence(device, target.fence, nullptr);
		}
		for (buffer &buf : data)
			destroy_buffer(buf);
		for (stream &s : tmu)
		{
			destroy_buffer(s.host);
			destroy_buffer(s.device);
		}
		for (auto const &entry : pipelines)
			vkDestroyPipeline(device, entry.second, nullptr);
		if (descriptor_pool)
			vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
		if (pipeline_layout)
			vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		for (VkRenderPass pass : passes)
			if (pass)
				vkDestroyRenderPass(device, pass, nullptr);
	}
}


bool voodoo_vk::impl::init(std::string &error)
{
	// 64-bit integers for the W and texture iterators, dual-source blending
	// for the colour-before-fog blend factor
	VkPhysicalDeviceFeatures wanted{};
	wanted.shaderInt64 = VK_TRUE;
	wanted.dualSrcBlend = VK_TRUE;
	if (!init_device(error, &wanted))
		return false;
	if (!enabled_features().shaderInt64 || !enabled_features().dualSrcBlend)
	{
		error = "the GPU lacks shaderInt64 or dualSrcBlend";
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
		VkPipelineStageFlags const attachment_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkAccessFlags const attachment_access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = attachment_stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[0].dstStageMask = attachment_stages;
		dependencies[0].srcAccessMask = attachment_access | VK_ACCESS_TRANSFER_READ_BIT;
		dependencies[0].dstAccessMask = attachment_access;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = attachment_stages;
		dependencies[1].dstStageMask = attachment_stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[1].srcAccessMask = attachment_access;
		dependencies[1].dstAccessMask = attachment_access | VK_ACCESS_TRANSFER_READ_BIT;
		VkRenderPassCreateInfo pass_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		pass_info.attachmentCount = 2;
		pass_info.pAttachments = attachments;
		pass_info.subpassCount = 1;
		pass_info.pSubpasses = &subpass;
		pass_info.dependencyCount = 2;
		pass_info.pDependencies = dependencies;
		if (vkCreateRenderPass(device, &pass_info, nullptr, &passes[variant]) != VK_SUCCESS)
		{
			error = "vkCreateRenderPass failed";
			return false;
		}
	}

	// one set of storage buffers for everything, a small push constant block
	VkDescriptorSetLayoutBinding bindings[VKB_COUNT]{};
	for (int i = 0; i < VKB_COUNT; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = VKB_COUNT;
	set_info.pBindings = bindings;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
	VkPushConstantRange push_range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
	VkDescriptorPoolSize pool_size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VKB_COUNT };
	VkDescriptorPoolCreateInfo descriptor_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	descriptor_info.maxSets = 1;
	descriptor_info.poolSizeCount = 1;
	descriptor_info.pPoolSizes = &pool_size;
	vkCreateDescriptorPool(device, &descriptor_info, nullptr, &descriptor_pool);
	VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	alloc.descriptorPool = descriptor_pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &set_layout;
	if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS)
	{
		error = "descriptor set allocation failed";
		return false;
	}

	tri_vert = create_module(s_tri_vert, sizeof(s_tri_vert));
	tri_frag = create_module(s_tri_frag, sizeof(s_tri_frag));
	lfb_vert = create_module(s_lfb_vert, sizeof(s_lfb_vert));
	lfb_frag = create_module(s_lfb_frag, sizeof(s_lfb_frag));
	if (!tri_vert || !tri_frag || !lfb_vert || !lfb_frag)
	{
		error = "shader rejected";
		return false;
	}

	// a first pipeline checks that the shaders link
	if (!pipeline(KEY_RGB_WRITE) || !pipeline(KEY_LFB_COLOR))
	{
		error = "pipeline creation failed";
		return false;
	}

	// command buffers and fences of the batches
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

	lookup_data.resize(VKL_STATIC_SIZE);
	return true;
}


VkPipeline voodoo_vk::impl::pipeline(u32 key)
{
	auto const found = pipelines.find(key);
	if (found != pipelines.end())
		return found->second;

	bool const lfb = (key & KEY_LFB_COLOR) != 0;
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = lfb ? lfb_vert : tri_vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = lfb ? lfb_frag : tri_frag;
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
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depth_state{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blend{};
	if (key == KEY_LFB_COLOR)
	{
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
	}
	else if (key == KEY_LFB_DEPTH)
	{
		depth_state.depthTestEnable = VK_TRUE;
		depth_state.depthWriteEnable = VK_TRUE;
		depth_state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	}
	else
	{
		// the Voodoo depth functions are in Vulkan order (never, <, =, <=, >, !=, >=, always)
		if (key & KEY_DEPTH)
		{
			depth_state.depthTestEnable = VK_TRUE;
			depth_state.depthWriteEnable = (key & KEY_DEPTH_WRITE) ? VK_TRUE : VK_FALSE;
			depth_state.depthCompareOp = VkCompareOp(BIT(key, 12, 3));
		}
		if (key & KEY_BLEND)
		{
			blend.blendEnable = VK_TRUE;
			blend.srcColorBlendFactor = src_factor(BIT(key, 1, 4));
			blend.dstColorBlendFactor = dst_factor(BIT(key, 5, 4));
			blend.colorBlendOp = VK_BLEND_OP_ADD;
			blend.srcAlphaBlendFactor = (key & KEY_SRC_ALPHA_ONE) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
			blend.dstAlphaBlendFactor = (key & KEY_DST_ALPHA_ONE) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
			blend.alphaBlendOp = VK_BLEND_OP_ADD;
		}
		if (key & KEY_RGB_WRITE)
			blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
		if (key & KEY_ALPHA_WRITE)
			blend.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
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
	info.pDepthStencilState = &depth_state;
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = pipeline_layout;
	info.renderPass = passes[0];
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		result = VK_NULL_HANDLE;
	pipelines.emplace(key, result);
	return result;
}


bool voodoo_vk::impl::create_targets()
{
	for (int i = 0; i < 3; i++)
	{
		if (!create_image(color[i], width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
			return false;
		color_ready[i] = false;
	}
	if (!create_image(depth, width, height, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
		return false;
	depth_ready = false;
	for (int i = 0; i < 3; i++)
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
	readback_buffer = -1;
	return create_buffer(readback, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}


void voodoo_vk::impl::destroy_targets()
{
	for (int i = 0; i < 3; i++)
	{
		if (framebuffers[i])
			vkDestroyFramebuffer(device, framebuffers[i], nullptr);
		framebuffers[i] = VK_NULL_HANDLE;
		destroy_image(color[i]);
	}
	destroy_image(depth);
	destroy_buffer(readback);
	readback_buffer = -1;
}


bool voodoo_vk::impl::ensure_stream(stream &s, VkDeviceSize bytes)
{
	if (s.host.size >= bytes)
		return true;
	vkDeviceWaitIdle(device);
	destroy_buffer(s.host);
	destroy_buffer(s.device);
	VkDeviceSize const size = std::max<VkDeviceSize>(bytes * 3 / 2, 64 * 1024);
	descriptors_dirty = true;
	return create_buffer(s.host, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) &&
			create_device_buffer(s.device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}


bool voodoo_vk::impl::ensure_host(buffer &buf, VkDeviceSize bytes)
{
	if (buf.size >= bytes)
		return true;
	destroy_buffer(buf);
	return create_buffer(buf, std::max<VkDeviceSize>(bytes * 3 / 2, 64 * 1024), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
}


bool voodoo_vk::impl::ensure_device(buffer &buf, VkDeviceSize bytes)
{
	if (buf.size >= bytes)
		return true;
	wait_all();
	destroy_buffer(buf);
	descriptors_dirty = true;
	return create_device_buffer(buf, std::max<VkDeviceSize>(bytes * 3 / 2, 64 * 1024), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}


void voodoo_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void voodoo_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


void voodoo_vk::impl::reset_batch()
{
	commands.clear();
	record_data.clear();
	offset_data.clear();
	pixel_data.clear();
	lookup_data.resize(VKL_STATIC_SIZE);
	palettes.clear();
	last_texture[0] = last_texture[1] = nullptr;
	fog_index = ~0U;
	textured_pending = false;
}


//-------------------------------------------------
//  lookup_index - lookup buffer word of the texel
//  table of a texture: one of the static tables,
//  or a palette copied once per batch
//-------------------------------------------------

u32 voodoo_vk::impl::lookup_index(int tmu, rasterizer_texture const &tex)
{
	static u32 const s_static_offset[7] = { VKL_RGB332, VKL_ALPHA8, VKL_INT8, VKL_AI44, VKL_RGB565, VKL_ARGB1555, VKL_ARGB4444 };
	rgb_t const *const table = tex.lookup();
	for (int i = 0; i < 7; i++)
		if (table == static_tables[i])
			return s_static_offset[i];

	// palettes are identified by their content: the renderer reuses its slots
	if ((table == last_texture[tmu]) && !std::memcmp(&lookup_data[last_texture_lookup[tmu]], table, 256 * sizeof(u32)))
		return last_texture_lookup[tmu];

	u32 index = ~0U;
	for (palette const &entry : palettes)
		if ((entry.source == table) && !std::memcmp(&lookup_data[entry.index], table, 256 * sizeof(u32)))
		{
			index = entry.index;
			break;
		}
	if (index == ~0U)
	{
		index = u32(lookup_data.size());
		lookup_data.resize(index + 256);
		std::memcpy(&lookup_data[index], table, 256 * sizeof(u32));
		palettes.push_back({ table, index });
	}
	last_texture[tmu] = table;
	last_texture_lookup[tmu] = index;
	return index;
}


u32 voodoo_vk::impl::fog_table(u8 const *blend, u8 const *delta)
{
	if ((fog_index != ~0U) && !std::memcmp(fog_cache, blend, 64) && !std::memcmp(fog_cache + 64, delta, 64))
		return fog_index;
	std::memcpy(fog_cache, blend, 64);
	std::memcpy(fog_cache + 64, delta, 64);
	fog_index = u32(lookup_data.size());
	for (int i = 0; i < 64; i++)
		lookup_data.push_back(delta[i] | (blend[i] << 8));
	return fog_index;
}


void voodoo_vk::impl::texture_block(u32 *block, int tmu, rasterizer_texture const &tex, u32 texmode, s64 const *iter)
{
	block[VKT_TEXMODE] = texmode;
	block[VKT_LOOKUP] = lookup_index(tmu, tex);
	block[VKT_WMASK] = tex.wmask();
	block[VKT_HMASK] = tex.hmask();
	block[VKT_LODMIN] = u32(s32(tex.lodmin()));
	block[VKT_LODMAX] = u32(s32(tex.lodmax()));
	block[VKT_LODBIAS] = u32(s32(tex.lodbias()));
	block[VKT_LODMASK] = tex.lodmask();
	block[VKT_DETAILMAX] = u32(tex.detailmax());
	block[VKT_DETAILBIAS] = u32(tex.detailbias());
	block[VKT_DETAILSCALE] = tex.detailscale();
	block[VKT_RAMMASK] = tex.ram_mask();
	for (int lod = 0; lod < 9; lod++)
		block[VKT_LODOFFSET + lod] = tex.lodoffset(lod);
	for (int i = 0; i < 9; i++)
		put64(&block[VKT_ST + 2 * i], iter[i]);
}


VkRect2D voodoo_vk::impl::clip(s32 left, s32 top, s32 right, s32 bottom) const
{
	left = std::clamp<s32>(left, 0, width);
	right = std::clamp<s32>(right, left, width);
	top = std::clamp<s32>(top, 0, height);
	bottom = std::clamp<s32>(bottom, top, height);
	return { { left, top }, { u32(right - left), u32(bottom - top) } };
}


void voodoo_vk::impl::add(command const &cmd)
{
	readback_buffer = -1;

	// extend the previous command when nothing changes in between
	if (!commands.empty() && (cmd.type != CMD_CLEAR))
	{
		command &last = commands.back();
		if ((last.type == cmd.type) && (last.buffer == cmd.buffer) && (last.key == cmd.key) &&
				(last.first + last.count == cmd.first) && !std::memcmp(&last.rect, &cmd.rect, sizeof(VkRect2D)))
		{
			last.count += cmd.count;
			return;
		}
	}
	commands.push_back(cmd);

	// send batches to the GPU as they fill, without waiting: the GPU draws
	// while the emulation goes on
	if ((offset_data.size() >= 256) || (pixel_data.size() >= 2 * 16384) || (lookup_data.size() >= VKL_STATIC_SIZE + 65536))
		flush(-1, false);
}


void voodoo_vk::impl::begin_pass(VkCommandBuffer cmd, int buffer)
{
	int const variant = (color_ready[buffer] ? 0 : 1) | (depth_ready ? 0 : 2);
	VkClearValue clears[2];
	clears[0].color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
	clears[1].depthStencil = { 0.0f, 0 };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = passes[variant];
	pass.framebuffer = framebuffers[buffer];
	pass.renderArea = { { 0, 0 }, { u32(width), u32(height) } };
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	color_ready[buffer] = true;
	depth_ready = true;

	VkViewport const viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0, nullptr);
	struct { float inv_width, inv_height; s32 scale, pad; } const push = { 1.0f / width, 1.0f / height, scale, 0 };
	vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
}


//-------------------------------------------------
//  flush - send the pending work to the GPU, and
//  copy a buffer to the read-back buffer if asked;
//  waits for the GPU when reading back or asked
//-------------------------------------------------

bool voodoo_vk::impl::flush(int readback_index, bool wait)
{
	if (commands.empty() && (readback_index < 0))
		return true;
	if (!width)
	{
		reset_batch();
		return false;
	}

	// the slot's host buffers are reused once its last batch is done
	slot &sl = slots[next_slot];
	next_slot = (next_slot + 1) % SLOTS;
	wait_slot(sl);

	// size the buffers (a new device lookup buffer gets the static tables again)
	VkDeviceSize const bytes[DATA_COUNT] = {
			std::max<size_t>(record_data.size(), 1) * 4, std::max<size_t>(offset_data.size(), 1) * 4,
			lookup_data.size() * 4, std::max<size_t>(pixel_data.size(), 2) * 4 };
	VkBuffer const old_lookup = data[DATA_LOOKUP].handle;
	for (int i = 0; i < DATA_COUNT; i++)
		if (!ensure_device(data[i], bytes[i]) || !ensure_host(sl.host[i], bytes[i]))
			return false;
	if (data[DATA_LOOKUP].handle != old_lookup)
		static_uploaded = false;
	size_t const lookup_start = static_uploaded ? VKL_STATIC_SIZE : 0;
	for (int i = 0; i < 2; i++)
	{
		VkBuffer const old_tmu = tmu[i].host.handle;
		if (tmu_size[i] && !ensure_stream(tmu[i], tmu_size[i]))
			return false;
		if (tmu[i].host.handle != old_tmu)
		{
			std::fill(tmu_dirty[i].begin(), tmu_dirty[i].end(), 1);
			tmu_any_dirty[i] = true;
		}
	}
	std::memcpy(sl.host[DATA_RECORDS].mapped, record_data.data(), record_data.size() * 4);
	std::memcpy(sl.host[DATA_OFFSETS].mapped, offset_data.data(), offset_data.size() * 4);
	std::memcpy(reinterpret_cast<u32 *>(sl.host[DATA_LOOKUP].mapped) + lookup_start, &lookup_data[lookup_start], (lookup_data.size() - lookup_start) * 4);
	std::memcpy(sl.host[DATA_PIXELS].mapped, pixel_data.data(), pixel_data.size() * 4);
	for (buffer &buf : sl.host)
		flush_buffer(buf, 0, buf.size);

	// texture memory pages that changed; the batches in flight may still
	// copy from the host mirror
	std::vector<VkBufferCopy> tmu_regions[2];
	if (tmu_any_dirty[0] || tmu_any_dirty[1])
		wait_all();
	for (int i = 0; i < 2; i++)
	{
		if (!tmu_any_dirty[i])
			continue;
		u8 *const host = reinterpret_cast<u8 *>(tmu[i].host.mapped);
		for (u32 page = 0; page < tmu_dirty[i].size(); page++)
		{
			if (!tmu_dirty[i][page])
				continue;
			VkDeviceSize const offset = VkDeviceSize(page) << PAGE_SHIFT;
			VkDeviceSize const size = std::min<VkDeviceSize>(VkDeviceSize(1) << PAGE_SHIFT, tmu_size[i] - offset);
			std::memcpy(host + offset, tmu_ram[i] + offset, size);
			texture_bytes += size;
			if (!tmu_regions[i].empty() && (tmu_regions[i].back().srcOffset + tmu_regions[i].back().size == offset))
				tmu_regions[i].back().size += size;
			else
				tmu_regions[i].push_back({ offset, offset, size });
			tmu_dirty[i][page] = 0;
		}
		tmu_any_dirty[i] = false;
		flush_buffer(tmu[i].host, 0, tmu[i].host.size);
	}

	// point the descriptors at the device buffers (a missing TMU 1 reads
	// TMU 0); they cannot change under a batch in flight
	if (descriptors_dirty)
	{
		wait_all();
		buffer const *const sources[VKB_COUNT] = { &data[DATA_RECORDS], &data[DATA_OFFSETS], &data[DATA_LOOKUP], &tmu[0].device, tmu[1].device.handle ? &tmu[1].device : &tmu[0].device, &data[DATA_PIXELS] };
		VkDescriptorBufferInfo infos[VKB_COUNT];
		VkWriteDescriptorSet writes[VKB_COUNT];
		for (int i = 0; i < VKB_COUNT; i++)
		{
			infos[i] = { sources[i]->handle, 0, VK_WHOLE_SIZE };
			writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = 1;
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}
		vkUpdateDescriptorSets(device, VKB_COUNT, writes, 0, nullptr);
		descriptors_dirty = false;
	}

	VkCommandBuffer const cmd = sl.cmd;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	// uploads, once the previous batch is done reading the device buffers
	VkMemoryBarrier reused{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reused.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	reused.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reused, 0, nullptr, 0, nullptr);
	VkDeviceSize const copy_start[DATA_COUNT] = { 0, 0, lookup_start * 4, 0 };
	VkDeviceSize const copy_bytes[DATA_COUNT] = { record_data.size() * 4, offset_data.size() * 4, (lookup_data.size() - lookup_start) * 4, pixel_data.size() * 4 };
	for (int i = 0; i < DATA_COUNT; i++)
	{
		if (!copy_bytes[i])
			continue;
		VkBufferCopy const region{ copy_start[i], copy_start[i], copy_bytes[i] };
		vkCmdCopyBuffer(cmd, sl.host[i].handle, data[i].handle, 1, &region);
	}
	for (int i = 0; i < 2; i++)
		if (!tmu_regions[i].empty())
			vkCmdCopyBuffer(cmd, tmu[i].host.handle, tmu[i].device.handle, u32(tmu_regions[i].size()), tmu_regions[i].data());
	static_uploaded = true;
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);

	// replay the commands
	int current = -1;
	u32 bound = ~0U;
	for (command const &c : commands)
	{
		if (c.buffer != current)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			begin_pass(cmd, c.buffer);
			current = c.buffer;
			bound = ~0U;
		}
		if (c.type == CMD_CLEAR)
		{
			VkClearAttachment attachments[2];
			u32 count = 0;
			if (c.clear_color)
			{
				attachments[count] = { VK_IMAGE_ASPECT_COLOR_BIT, 0, {} };
				attachments[count].clearValue.color = { { BIT(c.color, 16, 8) / 255.0f, BIT(c.color, 8, 8) / 255.0f, BIT(c.color, 0, 8) / 255.0f, 1.0f } };
				count++;
			}
			if (c.clear_depth)
			{
				attachments[count] = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, {} };
				attachments[count].clearValue.depthStencil = { c.depth, 0 };
				count++;
			}
			VkClearRect const rect{ c.rect, 0, 1 };
			if (count && rect.rect.extent.width && rect.rect.extent.height)
				vkCmdClearAttachments(cmd, count, attachments, 1, &rect);
			continue;
		}

		u32 const key = (c.type == CMD_TRIANGLES) ? c.key : (c.type == CMD_LFB_COLOR) ? KEY_LFB_COLOR : KEY_LFB_DEPTH;
		VkPipeline const p = pipeline(key);
		if (!p || !c.rect.extent.width || !c.rect.extent.height)
			continue;
		if (key != bound)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
			bound = key;
		}
		vkCmdSetScissor(cmd, 0, 1, &c.rect);
		if (c.type == CMD_TRIANGLES)
			vkCmdDraw(cmd, c.count * 3, 1, c.first * 3, 0);
		else
			vkCmdDraw(cmd, c.count * 6, 1, c.first * 6, 0);
	}
	if (current >= 0)
		vkCmdEndRenderPass(cmd);

	// copy a colour buffer out
	if (readback_index >= 0)
	{
		if (!color_ready[readback_index])
		{
			begin_pass(cmd, readback_index);
			vkCmdEndRenderPass(cmd);
		}
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = color[readback_index].handle;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { u32(width), u32(height), 1 };
		vkCmdCopyImageToBuffer(cmd, color[readback_index].handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &region);
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
		host.buffer = readback.handle;
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
	reset_batch();
	if (!ok)
		return false;

	// the read-back needs the image
	if ((readback_index >= 0) || wait)
		wait_slot(sl);
	if (readback_index >= 0)
	{
		readbacks++;
		if (!readback.coherent)
		{
			VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
			range.memory = readback.memory;
			range.size = VK_WHOLE_SIZE;
			vkInvalidateMappedMemoryRanges(device, 1, &range);
		}
		readback_buffer = readback_index;
	}
	return true;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

std::unique_ptr<voodoo_vk> voodoo_vk::create(int scale, std::string &error)
{
	auto state = std::make_unique<impl>();
	state->scale = scale;
	if (!state->init(error))
		return nullptr;
	return std::unique_ptr<voodoo_vk>(new voodoo_vk(std::move(state)));
}


voodoo_vk::voodoo_vk(std::unique_ptr<impl> &&state)
	: m(std::move(state))
{
}


voodoo_vk::~voodoo_vk()
{
}


std::string const &voodoo_vk::device_name() const
{
	return m->name;
}


void voodoo_vk::set_static_lookups(rgb_t const *const tables[7])
{
	static u32 const s_offset[7] = { VKL_RGB332, VKL_ALPHA8, VKL_INT8, VKL_AI44, VKL_RGB565, VKL_ARGB1555, VKL_ARGB4444 };
	static u32 const s_size[7] = { 256, 256, 256, 256, 65536, 65536, 65536 };
	for (int i = 0; i < 7; i++)
	{
		m->static_tables[i] = tables[i];
		std::memcpy(&m->lookup_data[s_offset[i]], tables[i], s_size[i] * sizeof(u32));
	}
	m->static_uploaded = false;
}


void voodoo_vk::set_tmu_memory(int tmu, u8 const *ram, u32 size)
{
	m->tmu_ram[tmu] = ram;
	m->tmu_size[tmu] = size;
	m->tmu_dirty[tmu].assign((size + (1 << PAGE_SHIFT) - 1) >> PAGE_SHIFT, 1);
	m->tmu_any_dirty[tmu] = true;
}


void voodoo_vk::tmu_written(int tmu, u8 const *ptr, u32 bytes)
{
	impl &s = *m;
	if (!s.tmu_ram[tmu] || (ptr < s.tmu_ram[tmu]) || (ptr + bytes > s.tmu_ram[tmu] + s.tmu_size[tmu]))
		return;

	// triangles already recorded must see the old texels
	if (s.textured_pending)
		s.flush(-1, false);
	u32 const offset = u32(ptr - s.tmu_ram[tmu]);
	for (u32 page = offset >> PAGE_SHIFT; page <= ((offset + bytes - 1) >> PAGE_SHIFT); page++)
		s.tmu_dirty[tmu][page] = 1;
	s.tmu_any_dirty[tmu] = true;
}


void voodoo_vk::set_layout(u32 rowpixels, u32 rows)
{
	impl &s = *m;
	if ((rowpixels == s.rowpixels) && (rows == s.rows))
		return;
	s.flush(-1, false);
	s.wait_all();
	s.destroy_targets();
	s.rowpixels = rowpixels;
	s.rows = rows;
	s.width = rowpixels * s.scale;
	s.height = rows * s.scale;
	if (!s.width || !s.height || !s.create_targets())
	{
		s.destroy_targets();
		s.width = s.height = 0;
	}
}


void voodoo_vk::triangle(int buffer, bool has_aux, poly_data const &poly, voodoo_renderer::vertex_t const *vert, triangle_state const &state)
{
	impl &s = *m;
	if ((buffer < 0) || (buffer > 2) || !s.width)
		return;

	auto const fbzmode = poly.raster.fbzmode();
	auto const alphamode = poly.raster.alphamode();
	auto const fogmode = poly.raster.fogmode();
	u32 const generic = poly.raster.generic();
	bool const textured = (generic & (rasterizer_params::GENERIC_TEX0 | rasterizer_params::GENERIC_TEX1)) != 0;

	u32 const base = u32(s.record_data.size());
	s.record_data.resize(base + (textured ? VKR_SIZE_TEX : VKR_SIZE_NOTEX), 0);
	u32 *const r = &s.record_data[base];

	// the target rows are the frame buffer rows
	for (int i = 0; i < 3; i++)
	{
		float const y = fbzmode.y_origin() ? float(poly.yorigin + 1) - vert[i].y : vert[i].y;
		r[VKR_VX0 + 2 * i] = float_bits(vert[i].x);
		r[VKR_VX0 + 2 * i + 1] = float_bits(y);
	}
	r[VKR_AX] = u32(poly.ax);
	r[VKR_AY] = u32(poly.ay);
	r[VKR_YORIGIN] = u32(poly.yorigin);
	r[VKR_FBZCP] = poly.raster.fbzcp().raw();
	r[VKR_FBZMODE] = fbzmode.raw();
	r[VKR_ALPHAMODE] = alphamode.raw();
	r[VKR_FOGMODE] = fogmode.raw();
	r[VKR_GENERIC] = generic;
	r[VKR_COLOR0] = poly.color0;
	r[VKR_COLOR1] = poly.color1;
	r[VKR_CHROMAKEY] = poly.chromakey;
	r[VKR_CHROMARANGE] = state.chroma_range;
	r[VKR_FOGCOLOR] = poly.fogcolor;
	r[VKR_ZACOLOR] = poly.zacolor;
	r[VKR_STIPPLE] = poly.stipple;
	r[VKR_ALPHAREF] = poly.alpharef;
	r[VKR_FOGTABLE] = (fogmode.enable_fog() && !fogmode.fog_constant() && (fogmode.fog_zalpha() == 0)) ? s.fog_table(state.fogblend, state.fogdelta) : 0;
	r[VKR_MISC] = state.fogdelta_mask | (state.bilinear_mask << 8);
	r[VKR_TMUCONFIG] = state.tmu_config;
	s32 const start[5] = { poly.startr, poly.startg, poly.startb, poly.starta, poly.startz };
	s32 const ddx[5] = { poly.drdx, poly.dgdx, poly.dbdx, poly.dadx, poly.dzdx };
	s32 const ddy[5] = { poly.drdy, poly.dgdy, poly.dbdy, poly.dady, poly.dzdy };
	for (int i = 0; i < 5; i++)
	{
		r[VKR_START + i] = u32(start[i]);
		r[VKR_DDX + i] = u32(ddx[i]);
		r[VKR_DDY + i] = u32(ddy[i]);
	}
	put64(&r[VKR_W + 0], poly.startw);
	put64(&r[VKR_W + 2], poly.dwdx);
	put64(&r[VKR_W + 4], poly.dwdy);
	if (textured)
	{
		if (poly.tex0)
		{
			s64 const iter[9] = { poly.starts0, poly.startt0, poly.startw0, poly.ds0dx, poly.dt0dx, poly.dw0dx, poly.ds0dy, poly.dt0dy, poly.dw0dy };
			s.texture_block(&r[VKR_TMU0], 0, *poly.tex0, poly.raster.texmode0().raw(), iter);
		}
		if (poly.tex1)
		{
			s64 const iter[9] = { poly.starts1, poly.startt1, poly.startw1, poly.ds1dx, poly.dt1dx, poly.dw1dx, poly.ds1dy, poly.dt1dy, poly.dw1dy };
			s.texture_block(&r[VKR_TMU1], 1, *poly.tex1, poly.raster.texmode1().raw(), iter);
		}
		s.textured_pending = true;
	}

	// pipeline: blending, depth and write masks
	u32 key = 0;
	if (alphamode.alphablend())
	{
		key |= KEY_BLEND | (alphamode.srcrgbblend() << 1) | (alphamode.dstrgbblend() << 5);
		if (alphamode.srcalphablend() == 4)
			key |= KEY_SRC_ALPHA_ONE;
		if (alphamode.dstalphablend() == 4)
			key |= KEY_DST_ALPHA_ONE;
	}
	bool const planes = fbzmode.enable_alpha_planes();
	bool const depth_test = has_aux && !planes && fbzmode.enable_depthbuf();
	bool const depth_write = has_aux && !planes && fbzmode.aux_buffer_mask();
	if (depth_test || depth_write)
		key |= KEY_DEPTH | ((depth_test ? fbzmode.depth_function() : 7) << 12) | (depth_write ? KEY_DEPTH_WRITE : 0);
	if (fbzmode.rgb_buffer_mask())
		key |= KEY_RGB_WRITE;
	if (has_aux && planes && fbzmode.aux_buffer_mask())
		key |= KEY_ALPHA_WRITE;

	command cmd{};
	cmd.type = CMD_TRIANGLES;
	cmd.buffer = s8(buffer);
	cmd.key = key;
	cmd.rect = fbzmode.enable_clipping()
			? s.clip(poly.clipleft, poly.cliptop, poly.clipright, poly.clipbottom)
			: s.clip(0, 0, s.width, s.height);
	cmd.first = u32(s.offset_data.size());
	cmd.count = 1;
	s.offset_data.push_back(base);
	s.add(cmd);
}


void voodoo_vk::fastfill(int buffer, bool has_aux, poly_data const &poly)
{
	impl &s = *m;
	if ((buffer < 0) || (buffer > 2) || !s.width)
		return;

	auto const fbzmode = poly.raster.fbzmode();
	command cmd{};
	cmd.type = CMD_CLEAR;
	cmd.buffer = s8(buffer);
	cmd.clear_color = fbzmode.rgb_buffer_mask();
	cmd.clear_depth = fbzmode.aux_buffer_mask() && has_aux && !fbzmode.enable_alpha_planes();
	if (!cmd.clear_color && !cmd.clear_depth)
		return;

	// the clip rectangle is in screen rows: flip it with the Y origin
	s32 top = poly.cliptop;
	s32 bottom = poly.clipbottom;
	if (fbzmode.y_origin())
	{
		s32 const flipped = poly.yorigin + 1 - bottom;
		bottom = poly.yorigin + 1 - top;
		top = flipped;
	}
	cmd.rect = s.clip(poly.clipleft, top, poly.clipright, bottom);
	cmd.color = poly.color1;
	cmd.depth = float(poly.zacolor & 0xffff) * (1.0f / 65536.0f);
	s.add(cmd);
}


void voodoo_vk::write_color(int buffer, s32 x, s32 scry, u16 value)
{
	impl &s = *m;
	if ((buffer < 0) || (buffer > 2) || !s.width || (x < 0) || (scry < 0) || (x >= s32(s.rowpixels)) || (scry >= s32(s.rows)))
		return;
	command cmd{};
	cmd.type = CMD_LFB_COLOR;
	cmd.buffer = s8(buffer);
	cmd.rect = s.clip(0, 0, s.width, s.height);
	cmd.first = u32(s.pixel_data.size() / 2);
	cmd.count = 1;
	s.pixel_data.push_back(u32(x) | (u32(scry) << 16));
	s.pixel_data.push_back(value);
	s.add(cmd);
}


void voodoo_vk::write_depth(s32 x, s32 scry, u16 value)
{
	impl &s = *m;
	if (!s.width || (x < 0) || (scry < 0) || (x >= s32(s.rowpixels)) || (scry >= s32(s.rows)))
		return;

	// the depth image is shared: draw it through whichever colour buffer is current
	command cmd{};
	cmd.type = CMD_LFB_DEPTH;
	cmd.buffer = s.commands.empty() ? 0 : s.commands.back().buffer;
	cmd.rect = s.clip(0, 0, s.width, s.height);
	cmd.first = u32(s.pixel_data.size() / 2);
	cmd.count = 1;
	s.pixel_data.push_back(u32(x) | (u32(scry) << 16));
	s.pixel_data.push_back(value);
	s.add(cmd);
}


void voodoo_vk::fill_rows(int buffer, u32 first, u32 count, u16 value)
{
	impl &s = *m;
	if ((buffer < 0) || (buffer > 2) || !s.width)
		return;
	command cmd{};
	cmd.type = CMD_CLEAR;
	cmd.buffer = s8(buffer);
	cmd.clear_color = true;
	cmd.rect = s.clip(0, first * s.scale, s.width, (first + count) * s.scale);
	cmd.color = rgbexpand<5,6,5>(value, 11, 5, 0);
	s.add(cmd);
}


bool voodoo_vk::read(int buffer, bitmap_rgb32 &dest, rectangle const &hclip, s32 xoffs, s32 yoffs, u8 const *lut)
{
	impl &s = *m;
	if ((buffer < 0) || (buffer > 2) || !s.width)
		return false;
	if (((s.readback_buffer != buffer) || !s.commands.empty()) && !s.flush(buffer, true))
		return false;

	u32 const *const image = reinterpret_cast<u32 const *>(s.readback.mapped);
	for (s32 y = hclip.top(); y <= hclip.bottom(); y++)
	{
		u32 *const dst = &dest.pix(y);
		s32 const sy = y - yoffs;
		if ((sy < 0) || (sy >= s.height))
		{
			std::fill_n(&dst[hclip.left()], hclip.width(), rgb_t::black());
			continue;
		}
		// the columns inside the image, black around them
		s32 const left = std::clamp(hclip.left(), xoffs, xoffs + s.width);
		s32 const right = std::clamp(hclip.right() + 1, left, xoffs + s.width);
		std::fill(&dst[hclip.left()], &dst[left], rgb_t::black());
		std::fill(&dst[right], &dst[hclip.right() + 1], rgb_t::black());
		u32 const *const src = image + size_t(sy) * s.width - xoffs;
		if (!lut)
			std::copy(&src[left], &src[right], &dst[left]);
		else
		{
			for (s32 x = left; x < right; x++)
			{
				u32 const pixel = src[x];
				dst[x] = rgb_t(lut[BIT(pixel, 16, 8)], lut[256 + BIT(pixel, 8, 8)], lut[512 + BIT(pixel, 0, 8)]);
			}
		}
	}
	return true;
}

} // namespace voodoo
