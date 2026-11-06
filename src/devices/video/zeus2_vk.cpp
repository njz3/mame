// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    zeus2_vk.cpp

    Vulkan renderer for the Midway Zeus 2 at a higher resolution (mame3d).

    Triangles, frame buffer writes and read-backs of the rows shown are
    recorded on the CPU in emulation order and sent to the GPU in batches,
    without waiting.  Each batch uploads its records and the wave RAM pages
    written since the previous batch, draws into the frame buffer images
    (colour and depth) and may copy rows into a ring of read-back buffers.
    The rows shown are read back as soon as the display moves to them, so
    the screen update usually finds them ready.

***************************************************************************/

#include "emu.h"
#include "zeus2_vk.h"

#include "vk3d_context.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

#include "zeus2_vk_shaders/layout.h"

// shaders compiled with glslc from zeus2_vk_shaders (see NOTES.md)
u32 const s_tri_vert[] = {
#include "zeus2_vk_shaders/tri.vert.spv.inc"
};
u32 const s_tri_frag[] = {
#include "zeus2_vk_shaders/tri.frag.spv.inc"
};
u32 const s_span_vert[] = {
#include "zeus2_vk_shaders/span.vert.spv.inc"
};
u32 const s_span_frag[] = {
#include "zeus2_vk_shaders/span.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr u32 FRAME_PIXELS = 512 * 1024 * 2;    // native frame buffer
constexpr u32 WAVERAM_PAGE = 0x4000;            // wave RAM uploaded in pages
constexpr u32 WAVERAM_PAGES = WAVERAM_BYTES / WAVERAM_PAGE;
constexpr u32 PALETTE_SIZE = 0x100;
constexpr int SLOTS = 3;
constexpr int READBACKS = 3;
constexpr size_t BATCH_TRIS = 2048;
constexpr size_t BATCH_VALUES = 1 << 21;

enum command_type : u8 { CMD_TRIS, CMD_SPANS, CMD_COPY };

struct command
{
	command_type type;
	u8 variant;         // triangles: depth test | depth write << 1; spans: depth write
	u32 first;          // record or copy region
	u32 count;
	int entry;          // copy: read-back entry
	VkRect2D scissor;   // triangles: the clip in the render window
};

struct push_constants
{
	float inv_width, inv_height;
	u32 base, states, values;
	s32 scale;
};

// poly_manager::round_coordinate
s32 round_coordinate(float value)
{
	if (value >= float(INT_MAX))
		return INT_MAX;
	float const ipart = std::floor(value);
	if (ipart < float(INT_MIN))
		return INT_MIN;
	return s32(ipart) + ((value - ipart > 0.5f) ? 1 : 0);
}

u32 float_bits(float value)
{
	u32 bits;
	std::memcpy(&bits, &value, 4);
	return bits;
}

} // anonymous namespace


struct zeus2_vk::impl : vk3d_context
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
		bitmap_rgb32 view;      // over host, for the display
		u32 display = 0;        // frame buffer address shown
		rectangle visible;
		bool valid = false;     // holds those rows as the frame buffer has them
		s32 rows[2]{};          // image rows read, top .. bottom - 1, past the bottom when they wrap
		int slot = -1;          // batch that copies it
		bool ready = false;
	};

	int scale = 1;
	int row_shift = 9;
	int width = 0;              // frame buffer images
	int height = 0;
	bool failed = false;
	u32 const *frame_color = nullptr;
	s32 const *frame_depth = nullptr;
	u32 const *waveram = nullptr;
	u32 const *palette_table = nullptr;

	VkRenderPass passes[2]{};           // loaded, cleared
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipeline tri_pipelines[4]{};      // depth test | depth write << 1
	VkPipeline span_pipelines[2]{};     // depth write

	image color;
	image depth;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	bool frame_ready = false;

	buffer wave;
	std::vector<bool> wave_dirty;       // per wave RAM page
	bool tris_pending = false;          // triangles of the batch read wave RAM
	buffer data;
	bool descriptors_dirty = true;
	slot slots[SLOTS];
	int next_slot = 0;

	readback_entry readback[READBACKS];
	int readback_width = 0;
	int readback_height = 0;
	int next_readback = 0;
	int shown = -1;
	int prefetched = -1;
	int pending = -1;                   // entry of the frame read back band by band
	bool band_pending = false;
	bool resync_pending = true;

	bool palette_dirty = true;          // the palette is not in this batch yet
	u32 palette_offset = 0;
	bool have_state = false;
	u32 last_state[PS_SIZE]{};
	u32 last_state_index = 0;

	std::vector<u32> tris;
	std::vector<u32> states;
	std::vector<u32> spans;
	std::vector<u32> values;
	std::vector<VkBufferImageCopy> copies;
	std::vector<command> commands;

	// statistics, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u64 tri_count = 0;
	u64 write_count = 0;
	u32 flushes = 0;
	u32 waveram_flushes = 0;
	u32 prefetches = 0;
	u32 prefetch_hits = 0;
	u32 reused = 0;
	u32 frames = 0;

	~impl();

	bool init(std::string &error);
	VkPipeline make_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool depth_test, bool depth_write);
	bool create_targets();
	void destroy_targets();
	bool ensure_readback(int w, int h);
	void wait_slot(slot &target);
	void wait_all();
	void check_resync();
	void add_command(command_type type, u8 variant, u32 index, VkRect2D const &scissor);
	void add_span(u32 x, u32 y, u32 count, u32 value, u32 flags, bool depth_write);
	void add_triangle(u32 state_index, vertex const &a, vertex const &b, vertex const &c, rectangle const &clip, s32 winx, s32 winy, u8 variant);
	u32 state_record(state const &st);
	void touch(s32 top, s32 bottom);
	int pick_entry();
	void add_copy(int entry, u32 display, int top, int bottom, rectangle const &visible);
	void set_entry(int index, u32 display, rectangle const &visible, bool valid);
	bool matches(int index, u32 display, rectangle const &visible) const;
	void begin_pass(VkCommandBuffer cmd);
	bool flush();
	void complete(readback_entry &entry);
	bitmap_rgb32 *show(int index, rectangle const &visible);
};


//**************************************************************************
//  SETUP
//**************************************************************************

zeus2_vk::impl::~impl()
{
	if (flushes)
		osd_printf_verbose("zeus2_vk: %u batches (%u for wave RAM writes), %u frames (%u prefetched, %u prefetches, %u shown again), %u triangles, %u frame buffer writes, %.1f ms waiting for the GPU\n",
				flushes, waveram_flushes, frames, prefetch_hits, prefetches, reused, u32(tri_count), u32(write_count), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		destroy_targets();
		for (readback_entry &entry : readback)
			destroy_buffer(entry.host);
		for (slot &target : slots)
		{
			destroy_buffer(target.host);
			if (target.fence)
				vkDestroyFence(device, target.fence, nullptr);
		}
		destroy_buffer(wave);
		destroy_buffer(data);
		for (VkPipeline p : tri_pipelines)
			if (p)
				vkDestroyPipeline(device, p, nullptr);
		for (VkPipeline p : span_pipelines)
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


bool zeus2_vk::impl::init(std::string &error)
{
	// dual-source blending for the pixel ALU blends
	VkPhysicalDeviceFeatures wanted{};
	wanted.dualSrcBlend = VK_TRUE;
	if (!init_device(error, &wanted))
		return false;
	if (!enabled_features().dualSrcBlend)
	{
		error = "the GPU lacks dualSrcBlend";
		return false;
	}

	// render passes: the colour image stays in the general layout, so that
	// rows can be copied out between passes; cleared on first use
	for (int clear = 0; clear < 2; clear++)
	{
		VkAttachmentDescription attachments[2]{};
		attachments[0].format = COLOR_FORMAT;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
		attachments[1].format = DEPTH_FORMAT;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_GENERAL };
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
		if (vkCreateRenderPass(device, &info, nullptr, &passes[clear]) != VK_SUCCESS)
		{
			error = "vkCreateRenderPass failed";
			return false;
		}
	}

	// wave RAM, records and values
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
	VkDescriptorPoolSize const pool_size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
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

	VkShaderModule const tri_vert = create_module(s_tri_vert, sizeof(s_tri_vert));
	VkShaderModule const tri_frag = create_module(s_tri_frag, sizeof(s_tri_frag));
	VkShaderModule const span_vert = create_module(s_span_vert, sizeof(s_span_vert));
	VkShaderModule const span_frag = create_module(s_span_frag, sizeof(s_span_frag));
	if (!tri_vert || !tri_frag || !span_vert || !span_frag)
	{
		error = "shader module creation failed";
		return false;
	}
	for (int variant = 0; variant < 4; variant++)
		if (!(tri_pipelines[variant] = make_pipeline(tri_vert, tri_frag, variant & 1, variant & 2)))
		{
			error = "pipeline creation failed";
			return false;
		}
	for (int variant = 0; variant < 2; variant++)
		if (!(span_pipelines[variant] = make_pipeline(span_vert, span_frag, false, variant)))
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

	if (!create_device_buffer(wave, WAVERAM_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
	{
		error = "wave RAM allocation failed";
		return false;
	}
	wave_dirty.assign(WAVERAM_PAGES, true);
	return true;
}


VkPipeline zeus2_vk::impl::make_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool depth_test, bool depth_write)
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

	// depth <= stored depth when testing; without the test the comparison
	// always passes, so that the depth can still be written
	VkPipelineDepthStencilStateCreateInfo depth_state{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth_state.depthTestEnable = VK_TRUE;
	depth_state.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
	depth_state.depthCompareOp = depth_test ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

	// colour = output 0 + output 1 * destination
	VkPipelineColorBlendAttachmentState blend{};
	blend.blendEnable = VK_TRUE;
	blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
	blend.colorBlendOp = VK_BLEND_OP_ADD;
	blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC1_ALPHA;
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


bool zeus2_vk::impl::create_targets()
{
	// the frame buffer as an image: native rows of 1 << row_shift pixels,
	// each pixel a block of scale x scale pixels
	width = (1 << row_shift) * scale;
	height = (FRAME_PIXELS >> row_shift) * scale;
	if ((u32(std::max(width, height)) > properties.limits.maxImageDimension2D) ||
			!create_image(color, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_image(depth, width, height, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
		return false;
	VkImageView const views[2] = { color.view, depth.view };
	VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = passes[0];
	info.attachmentCount = 2;
	info.pAttachments = views;
	info.width = width;
	info.height = height;
	info.layers = 1;
	if (vkCreateFramebuffer(device, &info, nullptr, &framebuffer) != VK_SUCCESS)
		return false;
	frame_ready = false;
	resync_pending = true;
	return true;
}


void zeus2_vk::impl::destroy_targets()
{
	if (framebuffer)
		vkDestroyFramebuffer(device, framebuffer, nullptr);
	framebuffer = VK_NULL_HANDLE;
	destroy_image(color);
	destroy_image(depth);
}


bool zeus2_vk::impl::ensure_readback(int w, int h)
{
	if ((w <= readback_width) && (h <= readback_height))
		return true;

	// a larger visible area: everything read back so far is dropped
	wait_all();
	readback_width = std::max(w, readback_width);
	readback_height = std::max(h, readback_height);
	for (readback_entry &entry : readback)
	{
		destroy_buffer(entry.host);
		entry.valid = false;
		entry.slot = -1;
		entry.ready = false;
		if (!create_buffer(entry.host, VkDeviceSize(readback_width) * readback_height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			return false;
	}
	shown = prefetched = pending = -1;
	band_pending = false;
	return true;
}


void zeus2_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void zeus2_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


//-------------------------------------------------
//  recording
//-------------------------------------------------

void zeus2_vk::impl::check_resync()
{
	// the whole frame buffer from the native one, row by row
	if (!resync_pending)
		return;
	resync_pending = false;
	u32 const rowlen = 1 << row_shift;
	for (u32 row = 0; row < (FRAME_PIXELS >> row_shift); row++)
	{
		u32 const value = u32(values.size());
		for (u32 x = 0; x < rowlen; x++)
		{
			values.push_back(frame_color[row * rowlen + x]);
			values.push_back(u32(frame_depth[row * rowlen + x]));
		}
		add_span(0, row, rowlen, value, 0, true);
		if (values.size() >= BATCH_VALUES)
			flush();
	}
}


void zeus2_vk::impl::add_command(command_type type, u8 variant, u32 index, VkRect2D const &scissor)
{
	if (!commands.empty())
	{
		command &last = commands.back();
		if ((last.type == type) && (last.variant == variant) && (last.first + last.count == index) &&
				(last.scissor.offset.x == scissor.offset.x) && (last.scissor.offset.y == scissor.offset.y) &&
				(last.scissor.extent.width == scissor.extent.width) && (last.scissor.extent.height == scissor.extent.height))
		{
			last.count++;
			return;
		}
	}
	command cmd{};
	cmd.type = type;
	cmd.variant = variant;
	cmd.first = index;
	cmd.count = 1;
	cmd.entry = -1;
	cmd.scissor = scissor;
	commands.push_back(cmd);
}


void zeus2_vk::impl::add_span(u32 x, u32 y, u32 count, u32 value, u32 flags, bool depth_write)
{
	u32 const index = u32(spans.size() / SR_SIZE);
	spans.push_back(x);
	spans.push_back(y);
	spans.push_back(count);
	spans.push_back(value);
	spans.push_back(flags);
	touch(y * scale, (y + 1) * scale);
	add_command(CMD_SPANS, depth_write ? 1 : 0, index, VkRect2D{});
}


u32 zeus2_vk::impl::state_record(state const &st)
{
	u32 record[PS_SIZE];
	record[PS_FLAGS] = u32(st.mode) | (u32(st.texel_format) << PF_FORMAT_SHIFT) | (st.blend ? PF_BLEND : 0) | (st.depth_clear ? PF_DEPTH_CLEAR : 0);
	record[PS_TEXBASE] = (st.texbase * 8) & (WAVERAM_BYTES - 1);
	record[PS_TEXWIDTH] = st.texwidth;
	record[PS_SOLID] = st.solidcolor;
	record[PS_TRANS] = st.transcolor;
	record[PS_SRCALPHA] = st.src_alpha;
	record[PS_DSTALPHA] = st.dst_alpha;
	record[PS_ZMIN] = u32(st.zbuf_min);
	record[PS_PALETTE] = 0;

	// texels go through the palette as it is now
	if ((st.mode == MODE_ALPHA) || (st.mode == MODE_TEXTURE))
	{
		if (palette_dirty)
		{
			palette_offset = u32(values.size());
			values.insert(values.end(), palette_table, palette_table + PALETTE_SIZE);
			palette_dirty = false;
		}
		record[PS_PALETTE] = palette_offset;
	}

	if (have_state && !std::memcmp(record, last_state, sizeof(record)))
		return last_state_index;
	last_state_index = u32(states.size() / PS_SIZE);
	states.insert(states.end(), record, record + PS_SIZE);
	std::memcpy(last_state, record, sizeof(record));
	have_state = true;
	return last_state_index;
}


void zeus2_vk::impl::add_triangle(u32 state_index, vertex const &a, vertex const &b, vertex const &c, rectangle const &clip, s32 winx, s32 winy, u8 variant)
{
	// poly_manager::render_triangle: sorted by Y, the rows covered
	vertex const *v1 = &a;
	vertex const *v2 = &b;
	vertex const *v3 = &c;
	if (v2->y < v1->y)
		std::swap(v1, v2);
	if (v3->y < v2->y)
	{
		std::swap(v2, v3);
		if (v2->y < v1->y)
			std::swap(v1, v2);
	}
	s32 const top = std::max(round_coordinate(v1->y), clip.top());
	s32 const bottom = std::min(round_coordinate(v3->y), clip.bottom() + 1);
	if (bottom - top <= 0)
		return;

	// the columns the extents may cover
	float const minx = std::min(std::min(v1->x, v2->x), v3->x);
	float const maxx = std::max(std::max(v1->x, v2->x), v3->x);
	s32 const boxleft = std::max(round_coordinate(minx) - 1, clip.left());
	s32 const boxright = std::min(round_coordinate(maxx) + 2, clip.right() + 1);
	if (boxright <= boxleft)
		return;

	// slopes and parameter planes, as render_triangle computes them
	float const dxdy_v1v2 = (v2->y == v1->y) ? 0.0f : (v2->x - v1->x) / (v2->y - v1->y);
	float const dxdy_v1v3 = (v3->y == v1->y) ? 0.0f : (v3->x - v1->x) / (v3->y - v1->y);
	float const dxdy_v2v3 = (v3->y == v2->y) ? 0.0f : (v3->x - v2->x) / (v3->y - v2->y);
	float param_start[4], param_dpdx[4], param_dpdy[4];
	float const a00 = v2->y - v3->y;
	float const a01 = v3->x - v2->x;
	float const a02 = v2->x*v3->y - v3->x*v2->y;
	float const a10 = v3->y - v1->y;
	float const a11 = v1->x - v3->x;
	float const a12 = v3->x*v1->y - v1->x*v3->y;
	float const a20 = v1->y - v2->y;
	float const a21 = v2->x - v1->x;
	float const a22 = v1->x*v2->y - v2->x*v1->y;
	float const det = a02 + a12 + a22;
	if (std::abs(det) < 0.00001f)
	{
		for (int i = 0; i < 4; i++)
		{
			param_dpdx[i] = 0.0f;
			param_dpdy[i] = 0.0f;
			param_start[i] = v1->p[i];
		}
	}
	else
	{
		float const idet = 1.0f / det;
		for (int i = 0; i < 4; i++)
		{
			param_dpdx[i] = idet * (v1->p[i]*a00 + v2->p[i]*a10 + v3->p[i]*a20);
			param_dpdy[i] = idet * (v1->p[i]*a01 + v2->p[i]*a11 + v3->p[i]*a21);
			param_start[i] = idet * (v1->p[i]*a02 + v2->p[i]*a12 + v3->p[i]*a22);
		}
	}

	u32 const index = u32(tris.size() / TR_SIZE);
	size_t const base = tris.size();
	tris.resize(base + TR_SIZE);
	u32 *const r = &tris[base];
	r[TR_STATE] = state_index;
	r[TR_TOP] = top;
	r[TR_BOTTOM] = bottom;
	r[TR_CLIPLEFT] = clip.left();
	r[TR_CLIPRIGHT] = clip.right() + 1;
	r[TR_WINX] = winx;
	r[TR_WINY] = winy;
	r[TR_V1X] = float_bits(v1->x);
	r[TR_V1Y] = float_bits(v1->y);
	r[TR_V2X] = float_bits(v2->x);
	r[TR_V2Y] = float_bits(v2->y);
	r[TR_V3X] = float_bits(v3->x);
	r[TR_V3Y] = float_bits(v3->y);
	r[TR_DXDY13] = float_bits(dxdy_v1v3);
	r[TR_DXDY12] = float_bits(dxdy_v1v2);
	r[TR_DXDY23] = float_bits(dxdy_v2v3);
	for (int i = 0; i < 4; i++)
	{
		r[TR_START + i] = float_bits(param_start[i]);
		r[TR_DPDX + i] = float_bits(param_dpdx[i]);
		r[TR_DPDY + i] = float_bits(param_dpdy[i]);
	}
	touch(winy + top, winy + bottom);

	// the GPU fills the triangle: only within the clip, not all the frame buffer
	s32 const scissor_left = std::clamp(winx + clip.left(), 0, width);
	s32 const scissor_top = std::clamp(winy + clip.top(), 0, height);
	s32 const scissor_right = std::clamp(winx + clip.right() + 1, 0, width);
	s32 const scissor_bottom = std::clamp(winy + clip.bottom() + 1, 0, height);
	VkRect2D const scissor{ { scissor_left, scissor_top }, { u32(std::max(scissor_right - scissor_left, 0)), u32(std::max(scissor_bottom - scissor_top, 0)) } };
	add_command(CMD_TRIS, variant, index, scissor);
	tris_pending = true;
	tri_count++;
}


void zeus2_vk::impl::touch(s32 top, s32 bottom)
{
	// image rows drawn: a read-back holding them no longer matches
	for (int index : { prefetched, shown })
	{
		if (index < 0)
			continue;
		readback_entry &entry = readback[index];
		if (entry.valid &&
				(((top < entry.rows[1]) && (entry.rows[0] < bottom)) ||
				((top < entry.rows[1] - height) && (entry.rows[0] - height < bottom))))
			entry.valid = false;
	}
}


int zeus2_vk::impl::pick_entry()
{
	// not the frame being shown or built, preferably not the prefetched one
	int result = -1;
	for (int pass = 0; (pass < 2) && (result < 0); pass++)
		for (int i = 0; (i < READBACKS) && (result < 0); i++)
		{
			int const index = (next_readback + i) % READBACKS;
			if ((index != shown) && (index != pending) && (pass || (index != prefetched)))
				result = index;
		}
	next_readback = (result + 1) % READBACKS;
	if (result == prefetched)
		prefetched = -1;
	readback[result].valid = false;
	return result;
}


void zeus2_vk::impl::add_copy(int entry, u32 display, int top, int bottom, rectangle const &visible)
{
	// rows top .. bottom of the screen from the frame buffer at display, the
	// rows wrapping at the end of the frame buffer
	u32 const rowlen = 1 << row_shift;
	u32 const rows = FRAME_PIXELS >> row_shift;
	u32 const first_row = display >> row_shift;
	s32 const w = visible.width() * scale;
	s32 const x0 = s32((display & (rowlen - 1)) + visible.left()) * scale;
	s32 const cw = std::min(w, width - x0);
	if (cw <= 0)
		return;
	u32 const first = u32(copies.size());
	for (int y = top; y <= bottom; )
	{
		u32 const row = (first_row + y) % rows;
		int const n = std::min<int>(bottom + 1 - y, rows - row);
		VkBufferImageCopy copy{};
		copy.bufferOffset = VkDeviceSize(y - visible.top()) * scale * w * 4;
		copy.bufferRowLength = w;
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageOffset = { x0, s32(row) * scale, 0 };
		copy.imageExtent = { u32(cw), u32(n * scale), 1 };
		copies.push_back(copy);
		y += n;
	}
	command cmd{};
	cmd.type = CMD_COPY;
	cmd.first = first;
	cmd.count = u32(copies.size()) - first;
	cmd.entry = entry;
	commands.push_back(cmd);
}


void zeus2_vk::impl::set_entry(int index, u32 display, rectangle const &visible, bool valid)
{
	readback_entry &entry = readback[index];
	u32 const rows = FRAME_PIXELS >> row_shift;
	entry.display = display;
	entry.visible = visible;
	entry.rows[0] = s32(((display >> row_shift) + visible.top()) % rows) * scale;
	entry.rows[1] = entry.rows[0] + visible.height() * scale;
	entry.valid = valid;
}


bool zeus2_vk::impl::matches(int index, u32 display, rectangle const &visible) const
{
	if (index < 0)
		return false;
	readback_entry const &entry = readback[index];
	return entry.valid && (entry.display == display) && (entry.visible == visible);
}


void zeus2_vk::impl::begin_pass(VkCommandBuffer cmd)
{
	VkClearValue clears[2]{};
	clears[1].depthStencil = { 1.0f, 0 };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = passes[frame_ready ? 0 : 1];
	pass.framebuffer = framebuffer;
	pass.renderArea = { { 0, 0 }, { u32(width), u32(height) } };
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	frame_ready = true;
	VkViewport const viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
}


//-------------------------------------------------
//  flush - send the pending work to the GPU
//-------------------------------------------------

bool zeus2_vk::impl::flush()
{
	if (failed)
		return false;
	bool const waves = std::find(wave_dirty.begin(), wave_dirty.end(), true) != wave_dirty.end();
	if (commands.empty() && !waves)
		return true;

	int const slot_index = next_slot;
	slot &sl = slots[slot_index];
	next_slot = (next_slot + 1) % SLOTS;
	wait_slot(sl);

	// triangle records, state records, span records, values, then the wave RAM pages
	u32 const tri_base = 0;
	u32 const state_base = tri_base + u32(tris.size());
	u32 const span_base = state_base + u32(states.size());
	u32 const value_base = span_base + u32(spans.size());
	u32 const total = value_base + u32(values.size());
	VkDeviceSize const bytes = std::max<VkDeviceSize>(VkDeviceSize(total) * 4, 4);
	u32 dirty_pages = 0;
	for (bool dirty : wave_dirty)
		dirty_pages += dirty ? 1 : 0;
	VkDeviceSize const host_bytes = bytes + VkDeviceSize(dirty_pages) * WAVERAM_PAGE;
	if (data.size < bytes)
	{
		wait_all();
		destroy_buffer(data);
		if (!create_device_buffer(data, std::max<VkDeviceSize>(bytes * 3 / 2, 1 << 20), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
			return false;
		descriptors_dirty = true;
	}
	if (sl.host.size < host_bytes)
	{
		destroy_buffer(sl.host);
		if (!create_buffer(sl.host, std::max<VkDeviceSize>(host_bytes * 3 / 2, 1 << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
			return false;
	}
	u32 *const out = reinterpret_cast<u32 *>(sl.host.mapped);
	std::copy(tris.begin(), tris.end(), out + tri_base);
	std::copy(states.begin(), states.end(), out + state_base);
	std::copy(spans.begin(), spans.end(), out + span_base);
	std::copy(values.begin(), values.end(), out + value_base);
	std::vector<VkBufferCopy> wave_copies;
	VkDeviceSize staged = bytes;
	for (u32 page = 0; page < WAVERAM_PAGES; page++)
	{
		if (!wave_dirty[page])
			continue;
		std::memcpy(reinterpret_cast<u8 *>(sl.host.mapped) + staged, reinterpret_cast<u8 const *>(waveram) + page * WAVERAM_PAGE, WAVERAM_PAGE);
		if (!wave_copies.empty() && (wave_copies.back().dstOffset + wave_copies.back().size == VkDeviceSize(page) * WAVERAM_PAGE))
			wave_copies.back().size += WAVERAM_PAGE;
		else
			wave_copies.push_back({ staged, VkDeviceSize(page) * WAVERAM_PAGE, WAVERAM_PAGE });
		staged += WAVERAM_PAGE;
		wave_dirty[page] = false;
	}
	flush_buffer(sl.host, 0, host_bytes);

	if (descriptors_dirty)
	{
		wait_all();
		buffer const *const sources[2] = { &wave, &data };
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

	// uploads, once the previous batches are done reading
	VkMemoryBarrier reused_memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reused_memory.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	reused_memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reused_memory, 0, nullptr, 0, nullptr);
	VkBufferCopy const region{ 0, 0, bytes };
	vkCmdCopyBuffer(cmd, sl.host.handle, data.handle, 1, &region);
	if (!wave_copies.empty())
		vkCmdCopyBuffer(cmd, sl.host.handle, wave.handle, u32(wave_copies.size()), wave_copies.data());
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);

	// replay
	bool in_pass = false;
	bool copied = false;
	VkPipeline bound = VK_NULL_HANDLE;
	for (command const &c : commands)
	{
		if (c.type == CMD_COPY)
		{
			if (in_pass)
				vkCmdEndRenderPass(cmd);
			in_pass = false;
			if (!frame_ready)
			{
				begin_pass(cmd);
				vkCmdEndRenderPass(cmd);
			}
			vkCmdCopyImageToBuffer(cmd, color.handle, VK_IMAGE_LAYOUT_GENERAL, readback[c.entry].host.handle, c.count, &copies[c.first]);
			readback[c.entry].slot = slot_index;
			readback[c.entry].ready = false;
			copied = true;
			continue;
		}
		if (!in_pass)
		{
			begin_pass(cmd);
			in_pass = true;
			bound = VK_NULL_HANDLE;
		}
		VkRect2D const whole{ { 0, 0 }, { u32(width), u32(height) } };
		vkCmdSetScissor(cmd, 0, 1, (c.type == CMD_TRIS) ? &c.scissor : &whole);
		VkPipeline const pipeline = (c.type == CMD_TRIS) ? tri_pipelines[c.variant] : span_pipelines[c.variant];
		if (pipeline != bound)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound = pipeline;
		}
		u32 const base = (c.type == CMD_TRIS) ? (tri_base + c.first * TR_SIZE) : (span_base + c.first * SR_SIZE);
		push_constants const push = { 1.0f / width, 1.0f / height, base, state_base, value_base, scale };
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd, c.count * ((c.type == CMD_TRIS) ? 3 : 6), 1, 0, 0);
	}
	if (in_pass)
		vkCmdEndRenderPass(cmd);
	if (copied)
	{
		VkMemoryBarrier host{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
	}
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vkResetFences(device, 1, &sl.fence);
	bool const ok = vkQueueSubmit(queue, 1, &submit, sl.fence) == VK_SUCCESS;
	sl.busy = ok;
	flushes++;
	commands.clear();
	tris.clear();
	states.clear();
	spans.clear();
	values.clear();
	copies.clear();
	tris_pending = false;
	have_state = false;
	palette_dirty = true;
	if (!ok)
		failed = true;
	return ok;
}


void zeus2_vk::impl::complete(readback_entry &entry)
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


bitmap_rgb32 *zeus2_vk::impl::show(int index, rectangle const &visible)
{
	// high-resolution screen pixel (x, y) is read-back pixel (x - left, y - top)
	readback_entry &entry = readback[index];
	complete(entry);
	s32 const left = visible.left() * scale;
	s32 const top = visible.top() * scale;
	s32 const w = visible.width() * scale;
	s32 const h = visible.height() * scale;
	u32 *const base = reinterpret_cast<u32 *>(entry.host.mapped) - (ptrdiff_t(top) * w + left);
	entry.view.wrap(base, left + w, top + h, w);
	shown = index;
	frames++;
	return &entry.view;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

std::unique_ptr<zeus2_vk> zeus2_vk::create(int scale, u32 const *frame_color, s32 const *frame_depth, u32 const *waveram, u32 const *palette, std::string &error)
{
	auto state = std::make_unique<impl>();
	if (!state->init(error))
		return nullptr;
	state->scale = scale;
	state->frame_color = frame_color;
	state->frame_depth = frame_depth;
	state->waveram = waveram;
	state->palette_table = palette;
	if (!state->create_targets())
	{
		error = "frame buffer allocation failed";
		return nullptr;
	}
	return std::unique_ptr<zeus2_vk>(new zeus2_vk(std::move(state)));
}


zeus2_vk::zeus2_vk(std::unique_ptr<impl> &&p)
	: m(std::move(p))
{
}


zeus2_vk::~zeus2_vk()
{
}


std::string const &zeus2_vk::device_name() const
{
	return m->name;
}


void zeus2_vk::set_layout(int row_shift)
{
	impl &s = *m;
	if ((row_shift == s.row_shift) || s.failed)
		return;

	// the images take the new row length; the work recorded so far is drawn
	// with the old one first
	s.flush();
	s.wait_all();
	s.destroy_targets();
	s.row_shift = row_shift;
	for (impl::readback_entry &entry : s.readback)
		entry.valid = false;
	if (!s.create_targets())
		s.failed = true;
}


void zeus2_vk::polygon(state const &st, u32 window, rectangle const &clip, int numverts, vertex const *v)
{
	impl &s = *m;
	if (s.failed || (numverts < 3))
		return;
	s.check_resync();

	// render_triangle_fan at the high resolution, in the render window
	int const scale = s.scale;
	rectangle const hclip(clip.left() * scale, (clip.right() + 1) * scale - 1, clip.top() * scale, (clip.bottom() + 1) * scale - 1);
	vertex hv[16];
	numverts = std::min<int>(numverts, std::size(hv));
	for (int i = 0; i < numverts; i++)
	{
		hv[i] = v[i];
		hv[i].x *= scale;
		hv[i].y *= scale;
	}
	window &= FRAME_PIXELS - 1;
	s32 const winx = s32(window & ((1 << s.row_shift) - 1)) * scale;
	s32 const winy = s32(window >> s.row_shift) * scale;
	u32 const state_index = s.state_record(st);
	u8 const variant = (st.depth_test ? 1 : 0) | ((st.depth_write && (st.mode != MODE_RGB555)) ? 2 : 0);
	for (int i = 2; i < numverts; i++)
		s.add_triangle(state_index, hv[0], hv[i - 1], hv[i], hclip, winx, winy, variant);

	// send batches to the GPU as they fill, without waiting
	if ((s.tris.size() >= BATCH_TRIS * TR_SIZE) || (s.values.size() >= BATCH_VALUES))
		s.flush();
}


void zeus2_vk::fill(u32 addr, u32 count, u32 color, s32 depth)
{
	impl &s = *m;
	if (s.failed)
		return;
	s.check_resync();

	// one value pair for runs of whole rows
	count = std::min(count, FRAME_PIXELS);
	addr &= FRAME_PIXELS - 1;
	u32 const value = u32(s.values.size());
	s.values.push_back(color);
	s.values.push_back(u32(depth));
	u32 const rowlen = 1 << s.row_shift;
	while (count > 0)
	{
		u32 const col = addr & (rowlen - 1);
		u32 const n = std::min(count, rowlen - col);
		s.add_span(col, addr >> s.row_shift, n, value, SF_CONSTANT, true);
		addr = (addr + n) & (FRAME_PIXELS - 1);
		count -= n;
	}
	s.write_count++;
	if (s.values.size() >= BATCH_VALUES)
		s.flush();
}


void zeus2_vk::pixel(u32 addr, u32 color, bool write_depth, s32 depth)
{
	impl &s = *m;
	if (s.failed)
		return;
	s.check_resync();
	addr &= FRAME_PIXELS - 1;
	u32 const x = addr & ((1 << s.row_shift) - 1);
	u32 const y = addr >> s.row_shift;
	s.write_count++;

	// a write next to the previous one extends its span
	if (!s.commands.empty())
	{
		command const &last = s.commands.back();
		if ((last.type == CMD_SPANS) && (last.variant == (write_depth ? 1 : 0)))
		{
			u32 *const span = &s.spans[s.spans.size() - SR_SIZE];
			if (!(span[SR_FLAGS] & SF_CONSTANT) && (span[SR_Y] == y) && (span[SR_X] + span[SR_COUNT] == x))
			{
				span[SR_COUNT]++;
				s.values.push_back(color);
				s.values.push_back(u32(depth));
				s.touch(y * s.scale, (y + 1) * s.scale);
				return;
			}
		}
	}
	u32 const value = u32(s.values.size());
	s.values.push_back(color);
	s.values.push_back(u32(depth));
	s.add_span(x, y, 1, value, 0, write_depth);
	if (s.values.size() >= BATCH_VALUES)
		s.flush();
}


void zeus2_vk::waveram_written(offs_t offset)
{
	impl &s = *m;

	// the triangles waiting to be sent read wave RAM as it was
	if (s.tris_pending)
	{
		s.waveram_flushes++;
		s.flush();
	}
	s.wave_dirty[(offset & (WAVERAM_BYTES - 1)) / WAVERAM_PAGE] = true;
}


void zeus2_vk::palette_changed()
{
	m->palette_dirty = true;
}


void zeus2_vk::resync()
{
	impl &s = *m;
	s.resync_pending = true;
	s.palette_dirty = true;
	std::fill(s.wave_dirty.begin(), s.wave_dirty.end(), true);
	for (impl::readback_entry &entry : s.readback)
		entry.valid = false;
}


void zeus2_vk::prefetch(u32 display, rectangle const &visible)
{
	impl &s = *m;
	int const w = visible.width() * s.scale;
	int const h = visible.height() * s.scale;
	if (s.failed || s.band_pending || visible.empty() || (visible.left() < 0) || (visible.top() < 0) || (w > s.readback_width) || (h > s.readback_height))
		return;
	display &= FRAME_PIXELS - 1;
	if (s.matches(s.prefetched, display, visible) || s.matches(s.shown, display, visible))
		return;
	s.check_resync();
	int const index = s.pick_entry();
	s.add_copy(index, display, visible.top(), visible.bottom(), visible);
	if (!s.flush())
		return;
	s.set_entry(index, display, visible, true);
	s.prefetched = index;
	s.prefetches++;
}


bool zeus2_vk::update(u32 display, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame)
{
	impl &s = *m;
	frame = nullptr;
	int const w = visible.width() * s.scale;
	int const h = visible.height() * s.scale;
	if (s.failed || visible.empty() || (visible.left() < 0) || (visible.top() < 0) || !s.ensure_readback(w, h))
		return false;
	display &= FRAME_PIXELS - 1;
	int const top = std::max(cliprect.top(), visible.top());
	int const bottom = std::min(cliprect.bottom(), visible.bottom());
	bool const last = cliprect.bottom() >= visible.bottom();

	// a whole frame: the rows read back when the display moved to them, or
	// the frame shown last, if nothing was drawn into them since
	if (!s.band_pending && (top == visible.top()) && last)
	{
		for (int index : { s.prefetched, s.shown })
		{
			if (s.matches(index, display, visible))
			{
				if (index == s.prefetched)
					s.prefetch_hits++;
				else
					s.reused++;
				s.prefetched = -1;
				frame = s.show(index, visible);
				return true;
			}
		}
	}

	// read the rows back now, in order with the drawing; the frame is shown
	// once its bottom is reached
	s.check_resync();
	if (!s.band_pending)
		s.pending = s.pick_entry();
	if (top <= bottom)
		s.add_copy(s.pending, display, top, bottom, visible);
	if (!last)
	{
		s.band_pending = true;
		return true;
	}
	bool const several = s.band_pending;
	s.band_pending = false;
	s.prefetched = -1;
	int const index = s.pending;
	s.pending = -1;
	if (!s.flush())
		return false;
	s.set_entry(index, display, visible, !several);
	frame = s.show(index, visible);
	return true;
}
