// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    gaelco3d_vk.cpp

    Vulkan renderer for the Gaelco 3D hardware at a higher resolution
    (mame3d).

    Triangles and read-backs of the rows shown are recorded on the CPU in
    emulation order and sent to the GPU in batches, without waiting.  The
    frame buffer is never cleared by the hardware: the image keeps it
    between frames, upside down as the screen shows it.

***************************************************************************/

#include "emu.h"
#include "gaelco3d_vk.h"

#include "vk3d_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

#include "gaelco3d_vk_shaders/layout.h"

// shaders compiled with glslc from gaelco3d_vk_shaders (see NOTES.md)
u32 const s_poly_vert[] = {
#include "gaelco3d_vk_shaders/poly.vert.spv.inc"
};
u32 const s_poly_frag[] = {
#include "gaelco3d_vk_shaders/poly.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr u32 PALETTE_SIZE = 32768;
constexpr int SLOTS = 3;
constexpr int READBACKS = 3;
constexpr size_t BATCH_TRIS = 4096;

enum command_type : u8 { CMD_TRIS, CMD_COPY };

struct command
{
	command_type type;
	u8 variant;         // triangles: depth compared | blended << 1
	u32 first;          // record or copy region
	u32 count;
	int entry;          // copy: read-back entry
};

struct push_constants
{
	float inv_width, inv_height;
	u32 base, values;
	s32 height;
	u32 texture_size, mask_size, mask_offset;
};

// poly_manager::round_coordinate
s32 round_coordinate(float value)
{
	float const ipart = std::floor(value);
	return s32(ipart) + ((value - ipart > 0.5f) ? 1 : 0);
}

u32 float_bits(float value)
{
	u32 bits;
	std::memcpy(&bits, &value, 4);
	return bits;
}

} // anonymous namespace


struct gaelco3d_vk::impl : vk3d_context
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
		int slot = -1;          // batch that copies it
		bool ready = false;
	};

	int scale = 1;
	int width = 0;              // frame image
	int height = 0;
	bool failed = false;
	rgb_t const *palette_table = nullptr;
	u32 texture_size = 0;
	u32 mask_size = 0;

	VkRenderPass passes[2]{};           // loaded, cleared
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipeline pipelines[4]{};          // depth compared | blended << 1

	image color;
	image depth;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	bool frame_ready_flag = false;

	buffer texture;                     // texture bytes, then the mask bits
	buffer data;
	bool descriptors_dirty = true;
	slot slots[SLOTS];
	int next_slot = 0;

	readback_entry readback[READBACKS];
	int readback_width = 0;
	int readback_height = 0;
	int next_readback = 0;
	int shown = -1;
	int pending = -1;                   // entry of the frame read back band by band
	bool band_pending = false;

	bool palette_dirty = true;          // the palette is not in this batch yet
	u32 palette_offset = 0;

	std::vector<u32> tris;
	std::vector<u32> values;
	std::vector<VkBufferImageCopy> copies;
	std::vector<command> commands;

	// statistics, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u64 tri_count = 0;
	u32 flushes = 0;
	u32 frames = 0;

	~impl();

	bool init(u8 const *texram, u8 const *texmask, std::string &error);
	VkPipeline make_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool compare, bool blend);
	bool ensure_readback(int w, int h);
	void wait_slot(slot &target);
	void wait_all();
	void add_triangle(int mode, plane const &p, u32 palette, vertex const &a, vertex const &b, vertex const &c, rectangle const &clip, u8 variant);
	int pick_entry();
	void add_copy(int entry, int top, int bottom, rectangle const &visible);
	void begin_pass(VkCommandBuffer cmd);
	bool upload(VkDeviceSize offset, void const *source, VkDeviceSize size);
	bool flush();
	void complete(readback_entry &entry);
	bitmap_rgb32 *show(int index, rectangle const &visible);
};


//**************************************************************************
//  SETUP
//**************************************************************************

gaelco3d_vk::impl::~impl()
{
	if (flushes)
		osd_printf_verbose("gaelco3d_vk: %u batches, %u frames, %u triangles, %.1f ms waiting for the GPU\n",
				flushes, frames, u32(tri_count), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		if (framebuffer)
			vkDestroyFramebuffer(device, framebuffer, nullptr);
		destroy_image(color);
		destroy_image(depth);
		for (readback_entry &entry : readback)
			destroy_buffer(entry.host);
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


bool gaelco3d_vk::impl::init(u8 const *texram, u8 const *texmask, std::string &error)
{
	// dual-source blending for the 50% blend
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

	// texture, records and values
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

	VkShaderModule const vertex = create_module(s_poly_vert, sizeof(s_poly_vert));
	VkShaderModule const fragment = create_module(s_poly_frag, sizeof(s_poly_frag));
	if (!vertex || !fragment)
	{
		error = "shader module creation failed";
		return false;
	}
	for (int variant = 0; variant < 4; variant++)
		if (!(pipelines[variant] = make_pipeline(vertex, fragment, variant & 1, variant & 2)))
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

	// the frame and depth buffers
	if ((u32(std::max(width, height)) > properties.limits.maxImageDimension2D) ||
			!create_image(color, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_image(depth, width, height, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
	{
		error = "frame buffer allocation failed";
		return false;
	}
	VkImageView const views[2] = { color.view, depth.view };
	VkFramebufferCreateInfo fb_info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fb_info.renderPass = passes[0];
	fb_info.attachmentCount = 2;
	fb_info.pAttachments = views;
	fb_info.width = width;
	fb_info.height = height;
	fb_info.layers = 1;
	if (vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS)
	{
		error = "frame buffer allocation failed";
		return false;
	}

	// the texture ROM and its mask, as bits, uploaded once
	std::vector<u32> mask((mask_size + 31) / 32, 0);
	for (u32 i = 0; i < mask_size; i++)
		if (texmask[i])
			mask[i / 32] |= 1U << (i % 32);
	VkDeviceSize const mask_bytes = VkDeviceSize(mask.size()) * 4;
	if (!create_device_buffer(texture, texture_size + std::max<VkDeviceSize>(mask_bytes, 4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
			!upload(0, texram, texture_size) || !upload(texture_size, mask.data(), mask_bytes))
	{
		error = "texture upload failed";
		return false;
	}
	return true;
}


VkPipeline gaelco3d_vk::impl::make_pipeline(VkShaderModule vertex, VkShaderModule fragment, bool compare, bool blend)
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

	// depth < stored depth when compared, always written
	VkPipelineDepthStencilStateCreateInfo depth_state{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth_state.depthTestEnable = VK_TRUE;
	depth_state.depthWriteEnable = VK_TRUE;
	depth_state.depthCompareOp = compare ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS;

	// colour = output 0 + output 1 * destination
	VkPipelineColorBlendAttachmentState blend_state{};
	blend_state.blendEnable = blend ? VK_TRUE : VK_FALSE;
	blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_SRC1_COLOR;
	blend_state.colorBlendOp = VK_BLEND_OP_ADD;
	blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC1_ALPHA;
	blend_state.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blending.attachmentCount = 1;
	blending.pAttachments = &blend_state;
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


bool gaelco3d_vk::impl::upload(VkDeviceSize offset, void const *source, VkDeviceSize size)
{
	// through a host buffer, waiting for the copy
	buffer staging;
	if (!create_buffer(staging, std::max<VkDeviceSize>(size, 4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
		return false;
	std::memcpy(staging.mapped, source, size);
	flush_buffer(staging, 0, size);
	vkResetCommandBuffer(command_buffer, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(command_buffer, &begin);
	VkBufferCopy const region{ 0, offset, size };
	vkCmdCopyBuffer(command_buffer, staging.handle, texture.handle, 1, &region);
	vkEndCommandBuffer(command_buffer);
	bool const ok = submit_and_wait();
	destroy_buffer(staging);
	return ok;
}


bool gaelco3d_vk::impl::ensure_readback(int w, int h)
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
		entry.slot = -1;
		entry.ready = false;
		if (!create_buffer(entry.host, VkDeviceSize(readback_width) * readback_height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			return false;
	}
	shown = pending = -1;
	band_pending = false;
	return true;
}


void gaelco3d_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void gaelco3d_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


//-------------------------------------------------
//  recording
//-------------------------------------------------

void gaelco3d_vk::impl::add_triangle(int mode, plane const &p, u32 palette, vertex const &a, vertex const &b, vertex const &c, rectangle const &clip, u8 variant)
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

	float const dxdy_v1v2 = (v2->y == v1->y) ? 0.0f : (v2->x - v1->x) / (v2->y - v1->y);
	float const dxdy_v1v3 = (v3->y == v1->y) ? 0.0f : (v3->x - v1->x) / (v3->y - v1->y);
	float const dxdy_v2v3 = (v3->y == v2->y) ? 0.0f : (v3->x - v2->x) / (v3->y - v2->y);

	u32 const index = u32(tris.size() / GR_SIZE);
	size_t const base = tris.size();
	tris.resize(base + GR_SIZE);
	u32 *const r = &tris[base];
	r[GR_FLAGS] = u32(mode);
	r[GR_TOP] = top;
	r[GR_BOTTOM] = bottom;
	r[GR_BOXLEFT] = boxleft;
	r[GR_BOXRIGHT] = boxright;
	r[GR_CLIPLEFT] = clip.left();
	r[GR_CLIPRIGHT] = clip.right() + 1;
	r[GR_V1X] = float_bits(v1->x);
	r[GR_V1Y] = float_bits(v1->y);
	r[GR_V2X] = float_bits(v2->x);
	r[GR_V2Y] = float_bits(v2->y);
	r[GR_DXDY13] = float_bits(dxdy_v1v3);
	r[GR_DXDY12] = float_bits(dxdy_v1v2);
	r[GR_DXDY23] = float_bits(dxdy_v2v3);
	r[GR_TEX] = p.tex;
	r[GR_COLOR] = p.color;
	r[GR_PALETTE] = palette;
	r[GR_Z0] = float_bits(p.z0);
	r[GR_OOZ_BASE] = float_bits(p.ooz_base);
	r[GR_OOZ_DX] = float_bits(p.ooz_dx);
	r[GR_OOZ_DY] = float_bits(p.ooz_dy);
	r[GR_UOZ_BASE] = float_bits(p.uoz_base);
	r[GR_UOZ_DX] = float_bits(p.uoz_dx);
	r[GR_UOZ_DY] = float_bits(p.uoz_dy);
	r[GR_VOZ_BASE] = float_bits(p.voz_base);
	r[GR_VOZ_DX] = float_bits(p.voz_dx);
	r[GR_VOZ_DY] = float_bits(p.voz_dy);

	if (!commands.empty())
	{
		command &prev = commands.back();
		if ((prev.type == CMD_TRIS) && (prev.variant == variant) && (prev.first + prev.count == index))
		{
			prev.count++;
			tri_count++;
			return;
		}
	}
	command cmd{};
	cmd.type = CMD_TRIS;
	cmd.variant = variant;
	cmd.first = index;
	cmd.count = 1;
	cmd.entry = -1;
	commands.push_back(cmd);
	tri_count++;
}


int gaelco3d_vk::impl::pick_entry()
{
	// not the frame being shown
	int result = -1;
	for (int i = 0; (i < READBACKS) && (result < 0); i++)
	{
		int const index = (next_readback + i) % READBACKS;
		if (index != shown)
			result = index;
	}
	next_readback = (result + 1) % READBACKS;
	return result;
}


void gaelco3d_vk::impl::add_copy(int entry, int top, int bottom, rectangle const &visible)
{
	// screen rows are image rows: the image is upside down like the screen
	s32 const w = visible.width() * scale;
	VkBufferImageCopy copy{};
	copy.bufferOffset = VkDeviceSize(top - visible.top()) * scale * w * 4;
	copy.bufferRowLength = w;
	copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageOffset = { visible.left() * scale, top * scale, 0 };
	copy.imageExtent = { u32(w), u32((bottom + 1 - top) * scale), 1 };
	command cmd{};
	cmd.type = CMD_COPY;
	cmd.first = u32(copies.size());
	cmd.count = 1;
	cmd.entry = entry;
	copies.push_back(copy);
	commands.push_back(cmd);
}


void gaelco3d_vk::impl::begin_pass(VkCommandBuffer cmd)
{
	VkClearValue clears[2]{};
	clears[1].depthStencil = { 0.0f, 0 };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = passes[frame_ready_flag ? 0 : 1];
	pass.framebuffer = framebuffer;
	pass.renderArea = { { 0, 0 }, { u32(width), u32(height) } };
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	frame_ready_flag = true;
	VkViewport const viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
}


//-------------------------------------------------
//  flush - send the pending work to the GPU
//-------------------------------------------------

bool gaelco3d_vk::impl::flush()
{
	if (failed)
		return false;
	if (commands.empty())
		return true;

	int const slot_index = next_slot;
	slot &sl = slots[slot_index];
	next_slot = (next_slot + 1) % SLOTS;
	wait_slot(sl);

	// triangle records, then values
	u32 const tri_base = 0;
	u32 const value_base = tri_base + u32(tris.size());
	u32 const total = value_base + u32(values.size());
	VkDeviceSize const bytes = std::max<VkDeviceSize>(VkDeviceSize(total) * 4, 4);
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
	std::copy(tris.begin(), tris.end(), out + tri_base);
	std::copy(values.begin(), values.end(), out + value_base);
	flush_buffer(sl.host, 0, bytes);

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

	// upload, once the previous batches are done reading
	VkMemoryBarrier reused_memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reused_memory.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	reused_memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reused_memory, 0, nullptr, 0, nullptr);
	VkBufferCopy const region{ 0, 0, bytes };
	vkCmdCopyBuffer(cmd, sl.host.handle, data.handle, 1, &region);
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
			if (!frame_ready_flag)
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
		if (pipelines[c.variant] != bound)
		{
			bound = pipelines[c.variant];
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
		}
		push_constants const push = { 1.0f / width, 1.0f / height, tri_base + c.first * GR_SIZE, value_base, height, texture_size, mask_size, texture_size / 4 };
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd, c.count * 6, 1, 0, 0);
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
	values.clear();
	copies.clear();
	palette_dirty = true;
	if (!ok)
		failed = true;
	return ok;
}


void gaelco3d_vk::impl::complete(readback_entry &entry)
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


bitmap_rgb32 *gaelco3d_vk::impl::show(int index, rectangle const &visible)
{
	// high-resolution screen pixel (x, y) is read-back pixel (x - left, y - top);
	// the read-back is waited for in frame_ready()
	readback_entry &entry = readback[index];
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

std::unique_ptr<gaelco3d_vk> gaelco3d_vk::create(int scale, int width, int height, u8 const *texture, u32 texture_size, u8 const *texmask, u32 texmask_size, rgb_t const *palette, std::string &error)
{
	auto state = std::make_unique<impl>();
	state->scale = scale;
	state->width = width * scale;
	state->height = height * scale;
	state->texture_size = texture_size;
	state->mask_size = texmask_size;
	state->palette_table = palette;
	if (!state->init(texture, texmask, error))
		return nullptr;
	return std::unique_ptr<gaelco3d_vk>(new gaelco3d_vk(std::move(state)));
}


gaelco3d_vk::gaelco3d_vk(std::unique_ptr<impl> &&p)
	: m(std::move(p))
{
}


gaelco3d_vk::~gaelco3d_vk()
{
}


std::string const &gaelco3d_vk::device_name() const
{
	return m->name;
}


void gaelco3d_vk::polygon(int mode, plane const &p, int numverts, vertex const *v, rectangle const &clip)
{
	impl &s = *m;
	if (s.failed || (numverts < 3))
		return;

	// the scanline renderers read the palette as it is now
	if (s.palette_dirty)
	{
		s.palette_offset = u32(s.values.size());
		s.values.resize(s.values.size() + PALETTE_SIZE);
		std::memcpy(&s.values[s.palette_offset], s.palette_table, PALETTE_SIZE * 4);
		s.palette_dirty = false;
	}

	// the depth is compared unless the renderer writes it unconditionally or
	// the polygon's depth is negative (always below the buffer), blended
	// for the alpha renderer
	bool const compare = (mode != MODE_NOZ) && (p.z0 >= 0.0f);
	u8 const variant = (compare ? 1 : 0) | ((mode == MODE_ALPHA) ? 2 : 0);
	for (int i = 2; i < numverts; i++)
		s.add_triangle(mode, p, s.palette_offset, v[0], v[i - 1], v[i], clip, variant);

	// send batches to the GPU as they fill, without waiting
	if (s.tris.size() >= BATCH_TRIS * GR_SIZE)
		s.flush();
}


void gaelco3d_vk::palette_changed()
{
	m->palette_dirty = true;
}


bool gaelco3d_vk::update(rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame)
{
	impl &s = *m;
	frame = nullptr;
	int const w = visible.width() * s.scale;
	int const h = visible.height() * s.scale;
	if (s.failed || visible.empty() || (visible.left() < 0) || (visible.top() < 0) || (w > s.width) || (h > s.height) || !s.ensure_readback(w, h))
		return false;
	int const top = std::max(cliprect.top(), visible.top());
	int const bottom = std::min(cliprect.bottom(), visible.bottom());
	bool const last = cliprect.bottom() >= visible.bottom();

	// read the rows back in order with the drawing; the frame is shown once
	// its bottom is reached
	if (!s.band_pending)
		s.pending = s.pick_entry();
	if (top <= bottom)
		s.add_copy(s.pending, top, bottom, visible);
	if (!last)
	{
		s.band_pending = true;
		return true;
	}
	s.band_pending = false;
	int const index = s.pending;
	s.pending = -1;
	if (!s.flush())
		return false;
	frame = s.show(index, visible);
	return true;
}


void gaelco3d_vk::frame_ready()
{
	impl &s = *m;
	if (s.shown >= 0)
		s.complete(s.readback[s.shown]);
}
