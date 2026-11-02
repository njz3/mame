// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    tc0780fpa_vk.cpp

    Vulkan renderer for the Taito TC0780FPA at a higher resolution
    (mame3d).

    Triangles, quads, buffer swaps and screen mixes are recorded on the CPU
    in emulation order and sent to the GPU in batches, without waiting.
    Each batch uploads its records and the texture RAM pages written since
    the previous batch.  The screen is mixed into a display image band by
    band and read back once its bottom is reached.

***************************************************************************/

#include "emu.h"
#include "tc0780fpa_vk.h"

#include "vk3d_context.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

#include "tc0780fpa_vk_shaders/layout.h"

// shaders compiled with glslc from tc0780fpa_vk_shaders (see NOTES.md)
u32 const s_poly_vert[] = {
#include "tc0780fpa_vk_shaders/poly.vert.spv.inc"
};
u32 const s_poly_frag[] = {
#include "tc0780fpa_vk_shaders/poly.frag.spv.inc"
};
u32 const s_mix_vert[] = {
#include "tc0780fpa_vk_shaders/mix.vert.spv.inc"
};
u32 const s_mix_frag[] = {
#include "tc0780fpa_vk_shaders/mix.frag.spv.inc"
};

constexpr VkFormat PEN_FORMAT = VK_FORMAT_R16_UINT;
constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat DISPLAY_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr float CLEAR_DEPTH = 0xffff / 65536.0f;    // the depth a swap clears to
constexpr u32 TEXTURE_PAGE = 0x1000;                // texture RAM uploaded in pages
constexpr u32 TEXTURE_PAGES = TEXTURE_BYTES / TEXTURE_PAGE;
constexpr int SLOTS = 3;
constexpr int READBACKS = 3;
constexpr size_t BATCH_RECORDS = 1 << 18;           // words
constexpr size_t BATCH_VALUES = 1 << 21;

enum command_type : u8 { CMD_TRIS, CMD_QUADS, CMD_CLEAR, CMD_MIX };

struct push_constants
{
	float inv_width, inv_height;
	u32 base, values;
	s32 scale, kind;
	u32 palette, colors;
	s32 pens, pens_width, pens_height;
	s32 left, top;
	s32 native_left, native_top, native_width;
};

struct command
{
	command_type type;
	u8 target;          // primitives, clear: frame buffer
	u32 first;          // triangle or quad record
	u32 count;
	s32 top, bottom;    // mix: display rows
	s32 width;          // mix: display columns
	push_constants mix;
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


struct tc0780fpa_vk::impl : vk3d_context
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
	int width = 0;              // high-resolution frame buffers
	int height = 0;
	int work = 0;               // frame buffer drawn into, the other one is shown
	bool failed = false;

	VkRenderPass pen_passes[2]{};       // loaded, cleared
	VkRenderPass display_passes[2]{};
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipeline poly_pipeline = VK_NULL_HANDLE;
	VkPipeline mix_pipeline = VK_NULL_HANDLE;

	image pens[2];
	image depth;
	VkFramebuffer pen_framebuffers[2]{};
	bool pens_ready[2]{};
	image display;
	VkFramebuffer display_framebuffer = VK_NULL_HANDLE;
	int display_width = 0;
	int display_height = 0;
	bool display_ready = false;

	buffer texbuf;
	u8 const *texram = nullptr;
	std::vector<bool> texture_dirty;    // per texture RAM page
	bool prims_pending = false;         // primitives of the batch read texture RAM
	buffer data;
	bool descriptors_dirty = true;
	slot slots[SLOTS];
	int next_slot = 0;

	readback_entry readback[READBACKS];
	int next_readback = 0;
	int last_readback = -1;
	int shown = -1;
	bool band_pending = false;

	std::vector<u32> tris;
	std::vector<u32> quads;
	std::vector<u32> values;
	std::vector<command> commands;
	u32 palette_offset = 0;             // last palette of the batch
	u32 palette_colors = 0;

	// statistics, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u64 prim_count = 0;
	u32 flushes = 0;
	u32 texture_flushes = 0;
	u32 mixes = 0;
	u32 frames = 0;

	~impl();

	bool init(std::string &error);
	VkPipeline make_pipeline(u32 const *vertex, size_t vertex_size, u32 const *fragment, size_t fragment_size, VkRenderPass pass, bool depth_test);
	bool create_targets();
	bool ensure_display(int w, int h);
	void wait_slot(slot &target);
	void wait_all();
	void add_command(command_type type, u32 index);
	void add_triangle(vertex const *v, int mode, texture const &tex, rectangle const &clip);
	void add_quad(vertex const *v, int mode, texture const &tex, rectangle const &clip);
	u32 add_palette(rgb_t const *palette, u32 colors);
	bool mix(push_constants const &push, int top, int bottom, rectangle const &visible, bitmap_rgb32 *&frame);
	void begin_pass(VkCommandBuffer cmd, VkRenderPass const *variants, VkFramebuffer framebuffer, bool &ready, int w, int h);
	bool flush(int readback_width, int readback_height);
	void complete(readback_entry &entry);
	bitmap_rgb32 *show(int index, rectangle const &visible);
};


//**************************************************************************
//  SETUP
//**************************************************************************

tc0780fpa_vk::impl::~impl()
{
	if (flushes)
		osd_printf_verbose("tc0780fpa_vk: %u batches (%u for texture writes), %u frames, %u mixes, %u primitives, %.1f ms waiting for the GPU\n",
				flushes, texture_flushes, frames, mixes, u32(prim_count), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		for (int i = 0; i < 2; i++)
		{
			if (pen_framebuffers[i])
				vkDestroyFramebuffer(device, pen_framebuffers[i], nullptr);
			destroy_image(pens[i]);
		}
		destroy_image(depth);
		if (display_framebuffer)
			vkDestroyFramebuffer(device, display_framebuffer, nullptr);
		destroy_image(display);
		for (readback_entry &entry : readback)
			destroy_buffer(entry.host);
		for (slot &target : slots)
		{
			destroy_buffer(target.host);
			if (target.fence)
				vkDestroyFence(device, target.fence, nullptr);
		}
		destroy_buffer(texbuf);
		destroy_buffer(data);
		for (VkPipeline p : { poly_pipeline, mix_pipeline })
			if (p)
				vkDestroyPipeline(device, p, nullptr);
		if (layout)
			vkDestroyPipelineLayout(device, layout, nullptr);
		if (pool)
			vkDestroyDescriptorPool(device, pool, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		if (sampler)
			vkDestroySampler(device, sampler, nullptr);
		for (int i = 0; i < 2; i++)
		{
			if (pen_passes[i])
				vkDestroyRenderPass(device, pen_passes[i], nullptr);
			if (display_passes[i])
				vkDestroyRenderPass(device, display_passes[i], nullptr);
		}
	}
}


bool tc0780fpa_vk::impl::init(std::string &error)
{
	if (!init_device(error))
		return false;

	// render passes: the frame buffers stay in the general layout, so that
	// one can be drawn into while the other is read by the mix
	for (int target = 0; target < 2; target++)
	{
		for (int clear = 0; clear < 2; clear++)
		{
			VkAttachmentDescription attachments[2]{};
			attachments[0].format = target ? DISPLAY_FORMAT : PEN_FORMAT;
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
			subpass.pDepthStencilAttachment = target ? nullptr : &depth_ref;
			VkPipelineStageFlags const stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			VkAccessFlags const access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
			VkSubpassDependency dependencies[2]{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[0].dstStageMask = stages;
			dependencies[0].srcAccessMask = access | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			dependencies[0].dstAccessMask = access;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = stages;
			dependencies[1].dstStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[1].srcAccessMask = access;
			dependencies[1].dstAccessMask = access | VK_ACCESS_TRANSFER_READ_BIT;
			VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
			info.attachmentCount = target ? 1 : 2;
			info.pAttachments = attachments;
			info.subpassCount = 1;
			info.pSubpasses = &subpass;
			info.dependencyCount = 2;
			info.pDependencies = dependencies;
			if (vkCreateRenderPass(device, &info, nullptr, target ? &display_passes[clear] : &pen_passes[clear]) != VK_SUCCESS)
			{
				error = "vkCreateRenderPass failed";
				return false;
			}
		}
	}

	// texture RAM, records and values, the two frame buffers
	VkDescriptorSetLayoutBinding bindings[4]{};
	for (int i = 0; i < 4; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 4;
	set_info.pBindings = bindings;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
	VkPushConstantRange push_range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants) };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
	VkDescriptorPoolSize const pool_sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
	VkDescriptorPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 2;
	pool_info.pPoolSizes = pool_sizes;
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
	VkSamplerCreateInfo sampler_info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	vkCreateSampler(device, &sampler_info, nullptr, &sampler);

	poly_pipeline = make_pipeline(s_poly_vert, sizeof(s_poly_vert), s_poly_frag, sizeof(s_poly_frag), pen_passes[0], true);
	mix_pipeline = make_pipeline(s_mix_vert, sizeof(s_mix_vert), s_mix_frag, sizeof(s_mix_frag), display_passes[0], false);
	if (!poly_pipeline || !mix_pipeline)
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


VkPipeline tc0780fpa_vk::impl::make_pipeline(u32 const *vertex, size_t vertex_size, u32 const *fragment, size_t fragment_size, VkRenderPass pass, bool depth_test)
{
	VkShaderModule const vertex_module = create_module(vertex, vertex_size);
	VkShaderModule const fragment_module = create_module(fragment, fragment_size);
	if (!vertex_module || !fragment_module)
		return VK_NULL_HANDLE;
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex_module;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment_module;
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

	// depth <= stored depth, then written
	VkPipelineDepthStencilStateCreateInfo depth_state{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depth_state.depthTestEnable = VK_TRUE;
	depth_state.depthWriteEnable = VK_TRUE;
	depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

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
	info.pDepthStencilState = depth_test ? &depth_state : nullptr;
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = layout;
	info.renderPass = pass;
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return result;
}


bool tc0780fpa_vk::impl::create_targets()
{
	if ((u32(std::max(width, height)) > properties.limits.maxImageDimension2D) ||
			!create_image(depth, width, height, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
		return false;
	for (int i = 0; i < 2; i++)
	{
		if (!create_image(pens[i], width, height, PEN_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
			return false;
		VkImageView const views[2] = { pens[i].view, depth.view };
		VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		info.renderPass = pen_passes[0];
		info.attachmentCount = 2;
		info.pAttachments = views;
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (vkCreateFramebuffer(device, &info, nullptr, &pen_framebuffers[i]) != VK_SUCCESS)
			return false;
	}
	VkDescriptorImageInfo image_infos[2];
	VkWriteDescriptorSet writes[2]{};
	for (int i = 0; i < 2; i++)
	{
		image_infos[i] = { sampler, pens[i].view, VK_IMAGE_LAYOUT_GENERAL };
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = VB_PENS0 + i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &image_infos[i];
	}
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

	if (!create_device_buffer(texbuf, TEXTURE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
		return false;
	texture_dirty.assign(TEXTURE_PAGES, true);
	return true;
}


bool tc0780fpa_vk::impl::ensure_display(int w, int h)
{
	if ((w <= display_width) && (h <= display_height))
		return true;

	// a larger visible area: everything read back so far is dropped
	wait_all();
	if (display_framebuffer)
		vkDestroyFramebuffer(device, display_framebuffer, nullptr);
	display_framebuffer = VK_NULL_HANDLE;
	destroy_image(display);
	for (readback_entry &entry : readback)
	{
		destroy_buffer(entry.host);
		entry.slot = -1;
		entry.ready = false;
	}
	shown = last_readback = -1;
	band_pending = false;
	display_ready = false;
	display_width = std::max(w, display_width);
	display_height = std::max(h, display_height);
	if (!create_image(display, display_width, display_height, DISPLAY_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
		return false;
	VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	info.renderPass = display_passes[0];
	info.attachmentCount = 1;
	info.pAttachments = &display.view;
	info.width = display_width;
	info.height = display_height;
	info.layers = 1;
	if (vkCreateFramebuffer(device, &info, nullptr, &display_framebuffer) != VK_SUCCESS)
		return false;
	for (readback_entry &entry : readback)
		if (!create_buffer(entry.host, VkDeviceSize(display_width) * display_height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			return false;
	return true;
}


void tc0780fpa_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void tc0780fpa_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


//-------------------------------------------------
//  recording
//-------------------------------------------------

void tc0780fpa_vk::impl::add_command(command_type type, u32 index)
{
	if (!commands.empty())
	{
		command &prev = commands.back();
		if ((prev.type == type) && (prev.target == work) && (prev.first + prev.count == index))
		{
			prev.count++;
			return;
		}
	}
	command cmd{};
	cmd.type = type;
	cmd.target = u8(work);
	cmd.first = index;
	cmd.count = 1;
	commands.push_back(cmd);
}


void tc0780fpa_vk::impl::add_triangle(vertex const *v, int mode, texture const &tex, rectangle const &clip)
{
	// poly_manager::render_triangle at the high resolution: sorted by Y, the
	// rows covered
	vertex hv[3];
	for (int i = 0; i < 3; i++)
	{
		hv[i] = v[i];
		hv[i].x *= scale;
		hv[i].y *= scale;
	}
	rectangle const hclip(clip.left() * scale, (clip.right() + 1) * scale - 1, clip.top() * scale, (clip.bottom() + 1) * scale - 1);
	vertex const *v1 = &hv[0];
	vertex const *v2 = &hv[1];
	vertex const *v3 = &hv[2];
	if (v2->y < v1->y)
		std::swap(v1, v2);
	if (v3->y < v2->y)
	{
		std::swap(v2, v3);
		if (v2->y < v1->y)
			std::swap(v1, v2);
	}
	s32 const top = std::max(round_coordinate(v1->y), hclip.top());
	s32 const bottom = std::min(round_coordinate(v3->y), hclip.bottom() + 1);
	if (bottom - top <= 0)
		return;

	// the columns the extents may cover
	float const minx = std::min(std::min(v1->x, v2->x), v3->x);
	float const maxx = std::max(std::max(v1->x, v2->x), v3->x);
	s32 const boxleft = std::max(round_coordinate(minx) - 1, hclip.left());
	s32 const boxright = std::min(round_coordinate(maxx) + 2, hclip.right() + 1);
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
	r[PR_FLAGS] = u32(mode) | (u32(tex.wrap_x) << PF_WRAPX_SHIFT) | (u32(tex.wrap_y) << PF_WRAPY_SHIFT);
	r[PR_TEXBASE] = u32(tex.base_x) | (u32(tex.base_y) << 16);
	r[PR_TOP] = top;
	r[PR_BOTTOM] = bottom;
	r[PR_BOXLEFT] = boxleft;
	r[PR_BOXRIGHT] = boxright;
	r[PR_CLIPLEFT] = hclip.left();
	r[PR_CLIPRIGHT] = hclip.right() + 1;
	r[TR_V1X] = float_bits(v1->x);
	r[TR_V1Y] = float_bits(v1->y);
	r[TR_V2X] = float_bits(v2->x);
	r[TR_V2Y] = float_bits(v2->y);
	r[TR_DXDY13] = float_bits(dxdy_v1v3);
	r[TR_DXDY12] = float_bits(dxdy_v1v2);
	r[TR_DXDY23] = float_bits(dxdy_v2v3);
	for (int i = 0; i < 4; i++)
	{
		r[TR_START + i] = float_bits(param_start[i]);
		r[TR_DPDX + i] = float_bits(param_dpdx[i]);
		r[TR_DPDY + i] = float_bits(param_dpdy[i]);
	}
	add_command(CMD_TRIS, index);
	prims_pending = true;
	prim_count++;
}


void tc0780fpa_vk::impl::add_quad(vertex const *v, int mode, texture const &tex, rectangle const &clip)
{
	// poly_manager::render_polygon<4> at the high resolution: the rows
	// covered
	vertex hv[4];
	for (int i = 0; i < 4; i++)
	{
		hv[i] = v[i];
		hv[i].x *= scale;
		hv[i].y *= scale;
	}
	rectangle const hclip(clip.left() * scale, (clip.right() + 1) * scale - 1, clip.top() * scale, (clip.bottom() + 1) * scale - 1);
	float minx = hv[0].x;
	float maxx = hv[0].x;
	int minv = 0;
	int maxv = 0;
	for (int i = 1; i < 4; i++)
	{
		if (hv[i].y < hv[minv].y)
			minv = i;
		else if (hv[i].y > hv[maxv].y)
			maxv = i;
		minx = std::min(minx, hv[i].x);
		maxx = std::max(maxx, hv[i].x);
	}
	s32 const top = std::max(round_coordinate(hv[minv].y), hclip.top());
	s32 const bottom = std::min(round_coordinate(hv[maxv].y), hclip.bottom() + 1);
	if (bottom - top <= 0)
		return;

	// the edges forward and backward from the top vertex to the bottom one,
	// without the horizontal ones
	struct edge
	{
		vertex const *v1, *v2;
		float dxdy;
		float dpdy[4];
	};
	edge forward[3]{}, backward[3]{};
	int fcount = 0;
	int bcount = 0;
	for (int cur = minv; cur != maxv; cur = (cur + 1) & 3)
	{
		edge &e = forward[fcount];
		e.v1 = &hv[cur];
		e.v2 = &hv[(cur + 1) & 3];
		if (e.v1->y == e.v2->y)
			continue;
		float const ooy = 1.0f / (e.v2->y - e.v1->y);
		e.dxdy = (e.v2->x - e.v1->x) * ooy;
		for (int i = 0; i < 4; i++)
			e.dpdy[i] = (e.v2->p[i] - e.v1->p[i]) * ooy;
		fcount++;
	}
	for (int cur = minv; cur != maxv; cur = (cur + 3) & 3)
	{
		edge &e = backward[bcount];
		e.v1 = &hv[cur];
		e.v2 = &hv[(cur + 3) & 3];
		if (e.v1->y == e.v2->y)
			continue;
		float const ooy = 1.0f / (e.v2->y - e.v1->y);
		e.dxdy = (e.v2->x - e.v1->x) * ooy;
		for (int i = 0; i < 4; i++)
			e.dpdy[i] = (e.v2->p[i] - e.v1->p[i]) * ooy;
		bcount++;
	}
	if (!fcount || !bcount)
		return;
	bool const forward_left = ((forward[0].v1 == backward[0].v1) && (forward[0].dxdy < backward[0].dxdy)) ||
			((forward[0].v1 != backward[0].v1) && (forward[0].v1->x < backward[0].v1->x));
	edge const *const left = forward_left ? forward : backward;
	edge const *const right = forward_left ? backward : forward;
	int const lcount = forward_left ? fcount : bcount;
	int const rcount = forward_left ? bcount : fcount;

	// the columns the extents may cover
	s32 const boxleft = std::max(round_coordinate(minx) - 1, hclip.left());
	s32 const boxright = std::min(round_coordinate(maxx) + 2, hclip.right() + 1);
	if (boxright <= boxleft)
		return;

	u32 const index = u32(quads.size() / QR_SIZE);
	size_t const base = quads.size();
	quads.resize(base + QR_SIZE, 0);
	u32 *const r = &quads[base];
	r[PR_FLAGS] = u32(mode) | (u32(tex.wrap_x) << PF_WRAPX_SHIFT) | (u32(tex.wrap_y) << PF_WRAPY_SHIFT) | (u32(lcount) << PF_LCOUNT_SHIFT) | (u32(rcount) << PF_RCOUNT_SHIFT);
	r[PR_TEXBASE] = u32(tex.base_x) | (u32(tex.base_y) << 16);
	r[PR_TOP] = top;
	r[PR_BOTTOM] = bottom;
	r[PR_BOXLEFT] = boxleft;
	r[PR_BOXRIGHT] = boxright;
	r[PR_CLIPLEFT] = hclip.left();
	r[PR_CLIPRIGHT] = hclip.right() + 1;
	r[QR_MAXY] = float_bits(hv[maxv].y);
	for (int side = 0; side < 2; side++)
	{
		for (int i = 0; i < (side ? rcount : lcount); i++)
		{
			edge const &e = (side ? right : left)[i];
			u32 *const out = &r[QR_EDGES + (side * 3 + i) * QE_SIZE];
			out[QE_X] = float_bits(e.v1->x);
			out[QE_Y] = float_bits(e.v1->y);
			out[QE_Y2] = float_bits(e.v2->y);
			out[QE_DXDY] = float_bits(e.dxdy);
			for (int p = 0; p < 4; p++)
			{
				out[QE_P + p] = float_bits(e.v1->p[p]);
				out[QE_DPDY + p] = float_bits(e.dpdy[p]);
			}
		}
	}
	add_command(CMD_QUADS, index);
	prims_pending = true;
	prim_count++;
}


u32 tc0780fpa_vk::impl::add_palette(rgb_t const *palette, u32 colors)
{
	// the batch's last palette again if unchanged
	if ((palette_colors == colors) && !std::memcmp(&values[palette_offset], palette, colors * 4))
		return palette_offset;
	palette_offset = u32(values.size());
	palette_colors = colors;
	values.resize(values.size() + colors);
	std::memcpy(&values[palette_offset], palette, colors * 4);
	return palette_offset;
}


void tc0780fpa_vk::impl::begin_pass(VkCommandBuffer cmd, VkRenderPass const *variants, VkFramebuffer framebuffer, bool &ready, int w, int h)
{
	VkClearValue clears[2]{};
	clears[1].depthStencil = { CLEAR_DEPTH, 0 };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = variants[ready ? 0 : 1];
	pass.framebuffer = framebuffer;
	pass.renderArea = { { 0, 0 }, { u32(w), u32(h) } };
	pass.clearValueCount = 2;
	pass.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	ready = true;
	VkViewport const viewport{ 0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &pass.renderArea);
}


//-------------------------------------------------
//  flush - send the pending work to the GPU, and
//  copy the display image to a read-back buffer if
//  asked
//-------------------------------------------------

bool tc0780fpa_vk::impl::flush(int readback_width, int readback_height)
{
	if (failed)
		return false;
	bool const copy_out = (readback_width > 0) && (readback_height > 0);
	bool const textures = std::find(texture_dirty.begin(), texture_dirty.end(), true) != texture_dirty.end();
	if (commands.empty() && !copy_out && !textures)
		return true;

	int const slot_index = next_slot;
	slot &sl = slots[slot_index];
	next_slot = (next_slot + 1) % SLOTS;
	wait_slot(sl);

	// triangle records, quad records, values, then the texture RAM pages
	u32 const tri_base = 0;
	u32 const quad_base = tri_base + u32(tris.size());
	u32 const value_base = quad_base + u32(quads.size());
	u32 const total = value_base + u32(values.size());
	VkDeviceSize const bytes = std::max<VkDeviceSize>(VkDeviceSize(total) * 4, 4);
	u32 dirty_pages = 0;
	for (bool dirty : texture_dirty)
		dirty_pages += dirty ? 1 : 0;
	VkDeviceSize const host_bytes = bytes + VkDeviceSize(dirty_pages) * TEXTURE_PAGE;
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
	std::copy(quads.begin(), quads.end(), out + quad_base);
	std::copy(values.begin(), values.end(), out + value_base);
	std::vector<VkBufferCopy> texture_copies;
	VkDeviceSize staged = bytes;
	for (u32 page = 0; page < TEXTURE_PAGES; page++)
	{
		if (!texture_dirty[page])
			continue;
		std::memcpy(reinterpret_cast<u8 *>(sl.host.mapped) + staged, texram + page * TEXTURE_PAGE, TEXTURE_PAGE);
		if (!texture_copies.empty() && (texture_copies.back().dstOffset + texture_copies.back().size == VkDeviceSize(page) * TEXTURE_PAGE))
			texture_copies.back().size += TEXTURE_PAGE;
		else
			texture_copies.push_back({ staged, VkDeviceSize(page) * TEXTURE_PAGE, TEXTURE_PAGE });
		staged += TEXTURE_PAGE;
		texture_dirty[page] = false;
	}
	flush_buffer(sl.host, 0, host_bytes);

	if (descriptors_dirty)
	{
		wait_all();
		buffer const *const sources[2] = { &texbuf, &data };
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

	// uploads, once the previous batches are done reading and copying
	VkMemoryBarrier reused_memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reused_memory.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	reused_memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reused_memory, 0, nullptr, 0, nullptr);
	VkBufferCopy const region{ 0, 0, bytes };
	vkCmdCopyBuffer(cmd, sl.host.handle, data.handle, 1, &region);
	if (!texture_copies.empty())
		vkCmdCopyBuffer(cmd, sl.host.handle, texbuf.handle, u32(texture_copies.size()), texture_copies.data());
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);

	// replay
	int current = -1;
	for (command const &c : commands)
	{
		if (c.type == CMD_MIX)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			current = -1;
			for (int i = 0; i < 2; i++)
				if (!pens_ready[i])
				{
					begin_pass(cmd, pen_passes, pen_framebuffers[i], pens_ready[i], width, height);
					vkCmdEndRenderPass(cmd);
				}
			begin_pass(cmd, display_passes, display_framebuffer, display_ready, display_width, display_height);
			VkRect2D const band{ { 0, c.top }, { u32(c.width), u32(c.bottom - c.top) } };
			vkCmdSetScissor(cmd, 0, 1, &band);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mix_pipeline);
			push_constants push = c.mix;
			push.inv_width = 1.0f / display_width;
			push.inv_height = 1.0f / display_height;
			push.values = value_base;
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			continue;
		}
		if (c.type == CMD_CLEAR)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			current = -1;
			pens_ready[c.target] = false;
			begin_pass(cmd, pen_passes, pen_framebuffers[c.target], pens_ready[c.target], width, height);
			vkCmdEndRenderPass(cmd);
			continue;
		}
		if (c.target != current)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			begin_pass(cmd, pen_passes, pen_framebuffers[c.target], pens_ready[c.target], width, height);
			current = c.target;
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, poly_pipeline);
		}
		bool const is_quad = c.type == CMD_QUADS;
		push_constants push{};
		push.inv_width = 1.0f / width;
		push.inv_height = 1.0f / height;
		push.base = is_quad ? (quad_base + c.first * QR_SIZE) : (tri_base + c.first * TR_SIZE);
		push.values = value_base;
		push.scale = scale;
		push.kind = is_quad ? KIND_QUAD : KIND_TRIANGLE;
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd, c.count * 6, 1, 0, 0);
	}
	if (current >= 0)
		vkCmdEndRenderPass(cmd);

	// copy the display image out, not into the frame being shown
	int rb = -1;
	if (copy_out)
	{
		for (int i = 0; (i < READBACKS) && (rb < 0); i++)
		{
			int const index = (next_readback + i) % READBACKS;
			if (index != shown)
				rb = index;
		}
		next_readback = (rb + 1) % READBACKS;
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { u32(readback_width), u32(readback_height), 1 };
		vkCmdCopyImageToBuffer(cmd, display.handle, VK_IMAGE_LAYOUT_GENERAL, readback[rb].host.handle, 1, &copy);
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
	quads.clear();
	values.clear();
	palette_colors = 0;
	prims_pending = false;
	if (!ok)
	{
		failed = true;
		return false;
	}
	if (rb >= 0)
	{
		readback[rb].slot = slot_index;
		readback[rb].ready = false;
		last_readback = rb;
	}
	return true;
}


void tc0780fpa_vk::impl::complete(readback_entry &entry)
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


bitmap_rgb32 *tc0780fpa_vk::impl::show(int index, rectangle const &visible)
{
	// high-resolution screen pixel (x, y) is display pixel (x - left, y - top);
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


bool tc0780fpa_vk::impl::mix(push_constants const &push, int top, int bottom, rectangle const &visible, bitmap_rgb32 *&frame)
{
	// the rows of this update into the display image; the frame is read back
	// once its bottom is reached
	frame = nullptr;
	int const w = visible.width() * scale;
	int const h = visible.height() * scale;
	bool const last = bottom >= visible.bottom();
	top = std::max(top, visible.top());
	bottom = std::min(bottom, visible.bottom());
	if (top <= bottom)
	{
		command cmd{};
		cmd.type = CMD_MIX;
		cmd.top = (top - visible.top()) * scale;
		cmd.bottom = (bottom + 1 - visible.top()) * scale;
		cmd.width = w;
		cmd.mix = push;
		cmd.mix.scale = scale;
		cmd.mix.pens = work ^ 1;
		cmd.mix.pens_width = width;
		cmd.mix.pens_height = height;
		cmd.mix.left = visible.left() * scale;
		cmd.mix.top = visible.top() * scale;
		commands.push_back(cmd);
		mixes++;
	}
	if (!last)
	{
		band_pending = true;
		return true;
	}
	band_pending = false;
	if (!flush(w, h) || (last_readback < 0))
		return false;
	frame = show(last_readback, visible);
	return true;
}


//**************************************************************************
//  PUBLIC INTERFACE
//**************************************************************************

std::unique_ptr<tc0780fpa_vk> tc0780fpa_vk::create(int scale, int width, int height, u8 const *texture_ram, std::string &error)
{
	auto state = std::make_unique<impl>();
	if (!state->init(error))
		return nullptr;
	state->scale = scale;
	state->width = width * scale;
	state->height = height * scale;
	state->texram = texture_ram;
	if (!state->create_targets())
	{
		error = "frame buffer allocation failed";
		return nullptr;
	}
	return std::unique_ptr<tc0780fpa_vk>(new tc0780fpa_vk(std::move(state)));
}


tc0780fpa_vk::tc0780fpa_vk(std::unique_ptr<impl> &&p)
	: m(std::move(p))
{
}


tc0780fpa_vk::~tc0780fpa_vk()
{
}


std::string const &tc0780fpa_vk::device_name() const
{
	return m->name;
}


void tc0780fpa_vk::triangle(vertex const *v, int mode, texture const &tex, rectangle const &clip)
{
	impl &s = *m;
	if (s.failed)
		return;
	s.add_triangle(v, mode, tex, clip);

	// send batches to the GPU as they fill, without waiting
	if (s.tris.size() + s.quads.size() >= BATCH_RECORDS)
		s.flush(0, 0);
}


void tc0780fpa_vk::quad(vertex const *v, int mode, texture const &tex, rectangle const &clip)
{
	impl &s = *m;
	if (s.failed)
		return;
	s.add_quad(v, mode, tex, clip);
	if (s.tris.size() + s.quads.size() >= BATCH_RECORDS)
		s.flush(0, 0);
}


void tc0780fpa_vk::swap()
{
	// the frame buffer drawn becomes the one shown, the other one is cleared
	impl &s = *m;
	if (s.failed)
		return;
	s.work ^= 1;
	command cmd{};
	cmd.type = CMD_CLEAR;
	cmd.target = u8(s.work);
	s.commands.push_back(cmd);
	if (!s.band_pending)
		s.flush(0, 0);
}


void tc0780fpa_vk::texture_written(offs_t offset)
{
	impl &s = *m;

	// the primitives waiting to be sent read texture RAM as it was
	if (s.prims_pending)
	{
		s.texture_flushes++;
		s.flush(0, 0);
	}
	s.texture_dirty[(offset & (TEXTURE_BYTES - 1)) / TEXTURE_PAGE] = true;
}


void tc0780fpa_vk::resync()
{
	impl &s = *m;
	std::fill(s.texture_dirty.begin(), s.texture_dirty.end(), true);
}


void tc0780fpa_vk::frame_ready()
{
	impl &s = *m;
	if (s.shown >= 0)
		s.complete(s.readback[s.shown]);
}


bool tc0780fpa_vk::mix(bitmap_ind16 const &bottom, bitmap_ind16 const &top, rgb_t const *palette, u32 colors, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame)
{
	impl &s = *m;
	frame = nullptr;
	if (s.failed || visible.empty() || (visible.left() < 0) || (visible.top() < 0) || !s.ensure_display(visible.width() * s.scale, visible.height() * s.scale))
		return false;
	push_constants push{};
	push.palette = s.add_palette(palette, colors);
	push.colors = colors;

	// the native pixels of the rows of this update, bottom | top << 16
	int const first = std::max(cliprect.top(), visible.top());
	int const last = std::min(cliprect.bottom(), visible.bottom());
	push.base = u32(s.values.size());
	push.native_left = visible.left();
	push.native_top = first;
	push.native_width = visible.width();
	if (first <= last)
	{
		s.values.resize(s.values.size() + size_t(last + 1 - first) * visible.width());
		u32 *out = &s.values[push.base];
		for (int y = first; y <= last; y++)
		{
			u16 const *const under = &bottom.pix(y);
			u16 const *const over = &top.pix(y);
			for (int x = visible.left(); x <= visible.right(); x++)
				*out++ = u32(under[x]) | (u32(over[x]) << 16);
		}
	}
	return s.mix(push, cliprect.top(), cliprect.bottom(), visible, frame);
}
