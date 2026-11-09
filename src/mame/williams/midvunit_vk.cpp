// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    midvunit_vk.cpp

    Vulkan renderer for the Midway V-Unit at a higher resolution (mame3d).

    Quads, CPU writes and conversions for the display are recorded on the
    CPU in emulation order and sent to the GPU in batches, without waiting.
    Each batch uploads its records and the texture RAM pages written since
    the previous batch, draws into the page images (16-bit palette indices)
    and the display image, and may copy the display image into a ring of
    read-back buffers.  A page is converted as soon as it becomes visible,
    so the screen update usually finds it ready.

***************************************************************************/

#include "emu.h"
#include "midvunit_vk.h"

#include "vk3d_context.h"
#include "vk3d_share.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

#include "midvunit_vk_shaders/layout.h"

// shaders compiled with glslc from midvunit_vk_shaders (see NOTES.md)
u32 const s_quad_vert[] = {
#include "midvunit_vk_shaders/quad.vert.spv.inc"
};
u32 const s_quad_frag[] = {
#include "midvunit_vk_shaders/quad.frag.spv.inc"
};
u32 const s_span_vert[] = {
#include "midvunit_vk_shaders/span.vert.spv.inc"
};
u32 const s_span_frag[] = {
#include "midvunit_vk_shaders/span.frag.spv.inc"
};
u32 const s_present_vert[] = {
#include "midvunit_vk_shaders/present.vert.spv.inc"
};
u32 const s_present_frag[] = {
#include "midvunit_vk_shaders/present.frag.spv.inc"
};

constexpr VkFormat PAGE_FORMAT = VK_FORMAT_R16_UINT;
constexpr VkFormat DISPLAY_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr int NATIVE_SIZE = 512;            // a video RAM page is 512 x 512
constexpr u32 PENS = 0x8000;
constexpr u32 TEXTURE_PAGE = 0x1000;        // texture RAM uploaded in pages
constexpr u32 TEXTURE_PAGES = TEXTURE_BYTES / TEXTURE_PAGE;
constexpr u32 TEXTURE_WINDOW = 0x10100;     // bytes a textured quad may read from its base
constexpr int SLOTS = 3;
constexpr int READBACKS = 3;
constexpr size_t BATCH_QUADS = 1024;
constexpr size_t BATCH_VALUES = 1 << 20;

enum command_type : u8 { CMD_QUADS, CMD_SPANS, CMD_PRESENT };

struct command
{
	command_type type;
	u8 page;
	u32 first;          // quad, span or palette index
	u32 count;
	s32 top, bottom;    // present: display rows
	s32 width;          // present: display columns
	s32 xoffs;          // present: page column of display column 0
};

struct push_constants
{
	float inv_width, inv_height;
	u32 base, values;
	s32 scale, page, xoffs;
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


struct midvunit_vk::impl : vk3d_context
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
		int page = -1;          // page converted, -1 if none, -2 for a frame of several updates
		u32 version = 0;        // version of that page
		rectangle visible;
		std::vector<u32> pens;
		int slot = -1;          // batch that copies it
		bool ready = false;
		vk3d_share::target *shared = nullptr; // the frame is in this texture of the display, not in host
	};

	std::unique_ptr<vk3d_share> share;
	vk3d_share::target *finishing = nullptr; // texture of the frame the batch copies out
	bool sharing = false;       // the last frame went to a texture: nothing to convert ahead

	int scale = 1;
	int width = 0;              // page images
	int height = 0;
	u16 const *vram = nullptr;
	u8 const *texram = nullptr;

	VkRenderPass page_passes[2]{};      // loaded, cleared
	VkRenderPass display_passes[2]{};
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipeline quad_pipeline = VK_NULL_HANDLE;
	VkPipeline span_pipeline = VK_NULL_HANDLE;
	VkPipeline present_pipeline = VK_NULL_HANDLE;

	image pages[2];
	VkFramebuffer page_framebuffers[2]{};
	bool page_ready[2]{};
	image display;
	VkFramebuffer display_framebuffer = VK_NULL_HANDLE;
	int display_width = 0;
	int display_height = 0;
	bool display_ready = false;

	buffer texture;
	std::vector<bool> texture_dirty;    // per texture page
	u32 texture_low = ~u32(0);          // bytes the quads of the batch may read
	u32 texture_high = 0;
	buffer data;
	bool descriptors_dirty = true;
	slot slots[SLOTS];
	int next_slot = 0;

	readback_entry readback[READBACKS];
	int next_readback = 0;
	int last_readback = -1;
	int shown = -1;
	int prefetched = -1;
	bool band_pending = false;          // rows of the frame being shown are converted already
	u32 versions[2]{};
	bool resync_pending = true;

	std::vector<u32> quads;
	std::vector<u32> spans;
	std::vector<u32> values;
	std::vector<u32> palettes;
	std::vector<command> commands;

	// statistics, shown with -verbose
	osd_ticks_t wait_ticks = 0;
	u64 quad_count = 0;
	u64 write_count = 0;
	u32 flushes = 0;
	u32 texture_flushes = 0;
	u32 presents = 0;
	u32 prefetches = 0;
	u32 prefetch_hits = 0;
	u32 reused = 0;
	u32 frames = 0;

	~impl();

	bool init(std::string &error);
	VkPipeline make_pipeline(u32 const *vertex, size_t vertex_size, u32 const *fragment, size_t fragment_size, VkRenderPass pass);
	bool create_targets();
	bool ensure_display(int w, int h);
	void wait_slot(slot &target);
	void wait_all();
	void check_resync();
	void add_command(command_type type, int page, u32 index);
	void add_span(int page, int x, int y, int count, u16 const *src);
	void add_present(int page, int top, int bottom, rectangle const &visible, rgb_t const *pens);
	void begin_pass(VkCommandBuffer cmd, VkRenderPass const *variants, VkFramebuffer framebuffer, bool &ready, int w, int h);
	bool flush(int readback_width, int readback_height);
	void complete(readback_entry &entry);
	bool fits(rectangle const &visible) const;
	bool matches(readback_entry const &entry, int page, rectangle const &visible, rgb_t const *pens) const;
	void set_entry(int index, int page, rectangle const &visible, rgb_t const *pens);
	bitmap_rgb32 *show(int index, rectangle const &visible);
};


//**************************************************************************
//  SETUP
//**************************************************************************

midvunit_vk::impl::~impl()
{
	if (flushes)
		osd_printf_verbose("midvunit_vk: %u batches (%u for texture writes), %u frames (%u prefetched, %u prefetches, %u shown again), %u conversions, %u quads, %u CPU writes, %.1f ms waiting for the GPU\n",
				flushes, texture_flushes, frames, prefetch_hits, prefetches, reused, presents, u32(quad_count), u32(write_count), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		for (int i = 0; i < 2; i++)
		{
			if (page_framebuffers[i])
				vkDestroyFramebuffer(device, page_framebuffers[i], nullptr);
			destroy_image(pages[i]);
		}
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
		destroy_buffer(texture);
		destroy_buffer(data);
		for (VkPipeline p : { quad_pipeline, span_pipeline, present_pipeline })
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
			if (page_passes[i])
				vkDestroyRenderPass(device, page_passes[i], nullptr);
			if (display_passes[i])
				vkDestroyRenderPass(device, display_passes[i], nullptr);
		}
	}
}


bool midvunit_vk::impl::init(std::string &error)
{
	if (!init_device(error))
		return false;
	share = std::make_unique<vk3d_share>(*this);

	// render passes: the images stay in the general layout, so that a page
	// can be drawn into and read by the display conversion; cleared on
	// first use
	for (int target = 0; target < 2; target++)
	{
		for (int clear = 0; clear < 2; clear++)
		{
			VkAttachmentDescription attachment{};
			attachment.format = target ? DISPLAY_FORMAT : PAGE_FORMAT;
			attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
			attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
			VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_GENERAL };
			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &ref;
			VkPipelineStageFlags const stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			VkAccessFlags const access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
			VkSubpassDependency dependencies[2]{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[0].dstStageMask = stages;
			dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			dependencies[0].dstAccessMask = access;
			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependencies[1].dstStageMask = stages | VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = access | VK_ACCESS_TRANSFER_READ_BIT;
			VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
			info.attachmentCount = 1;
			info.pAttachments = &attachment;
			info.subpassCount = 1;
			info.pSubpasses = &subpass;
			info.dependencyCount = 2;
			info.pDependencies = dependencies;
			if (vkCreateRenderPass(device, &info, nullptr, target ? &display_passes[clear] : &page_passes[clear]) != VK_SUCCESS)
			{
				error = "vkCreateRenderPass failed";
				return false;
			}
		}
	}

	// texture RAM, frame data, the two pages
	VkDescriptorSetLayoutBinding bindings[3]{};
	for (int i = 0; i < 2; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	bindings[2].binding = VB_PAGES;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[2].descriptorCount = 2;
	bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 3;
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

	quad_pipeline = make_pipeline(s_quad_vert, sizeof(s_quad_vert), s_quad_frag, sizeof(s_quad_frag), page_passes[0]);
	span_pipeline = make_pipeline(s_span_vert, sizeof(s_span_vert), s_span_frag, sizeof(s_span_frag), page_passes[0]);
	present_pipeline = make_pipeline(s_present_vert, sizeof(s_present_vert), s_present_frag, sizeof(s_present_frag), display_passes[0]);
	if (!quad_pipeline || !span_pipeline || !present_pipeline)
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


VkPipeline midvunit_vk::impl::make_pipeline(u32 const *vertex, size_t vertex_size, u32 const *fragment, size_t fragment_size, VkRenderPass pass)
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
	info.layout = layout;
	info.renderPass = pass;
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return result;
}


bool midvunit_vk::impl::create_targets()
{
	for (int i = 0; i < 2; i++)
	{
		if (!create_image(pages[i], width, height, PAGE_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
			return false;
		VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		info.renderPass = page_passes[0];
		info.attachmentCount = 1;
		info.pAttachments = &pages[i].view;
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (vkCreateFramebuffer(device, &info, nullptr, &page_framebuffers[i]) != VK_SUCCESS)
			return false;
	}
	VkDescriptorImageInfo image_infos[2];
	for (int i = 0; i < 2; i++)
		image_infos[i] = { sampler, pages[i].view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = set;
	write.dstBinding = VB_PAGES;
	write.descriptorCount = 2;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = image_infos;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

	if (!create_device_buffer(texture, TEXTURE_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
		return false;
	texture_dirty.assign(TEXTURE_PAGES, true);
	return true;
}


bool midvunit_vk::impl::ensure_display(int w, int h)
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
		entry.page = -1;
		entry.slot = -1;
		entry.ready = false;
	}
	shown = prefetched = last_readback = -1;
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


void midvunit_vk::impl::wait_slot(slot &target)
{
	if (!target.busy)
		return;
	osd_ticks_t const start = osd_ticks();
	vkWaitForFences(device, 1, &target.fence, VK_TRUE, ~u64(0));
	wait_ticks += osd_ticks() - start;
	target.busy = false;
}


void midvunit_vk::impl::wait_all()
{
	for (slot &target : slots)
		wait_slot(target);
}


//-------------------------------------------------
//  recording
//-------------------------------------------------

void midvunit_vk::impl::check_resync()
{
	// both pages from the native video RAM, row by row
	if (!resync_pending)
		return;
	resync_pending = false;
	for (int page = 0; page < 2; page++)
		for (int y = 0; y < NATIVE_SIZE; y++)
			add_span(page, 0, y, NATIVE_SIZE, vram + (page * NATIVE_SIZE + y) * NATIVE_SIZE);
}


void midvunit_vk::impl::add_command(command_type type, int page, u32 index)
{
	versions[page]++;
	if (!commands.empty())
	{
		command &last = commands.back();
		if ((last.type == type) && (last.page == page) && (last.first + last.count == index))
		{
			last.count++;
			return;
		}
	}
	command cmd{};
	cmd.type = type;
	cmd.page = u8(page);
	cmd.first = index;
	cmd.count = 1;
	commands.push_back(cmd);
}


void midvunit_vk::impl::add_span(int page, int x, int y, int count, u16 const *src)
{
	u32 const index = u32(spans.size() / SR_SIZE);
	spans.push_back(x);
	spans.push_back(y);
	spans.push_back(count);
	spans.push_back(u32(values.size()));
	values.insert(values.end(), src, src + count);
	add_command(CMD_SPANS, page, index);
}


void midvunit_vk::impl::add_present(int page, int top, int bottom, rectangle const &visible, rgb_t const *pens)
{
	u32 const index = u32(palettes.size() / PENS);
	palettes.resize(palettes.size() + PENS);
	std::memcpy(&palettes[size_t(index) * PENS], pens, PENS * 4);
	command cmd{};
	cmd.type = CMD_PRESENT;
	cmd.page = u8(page);
	cmd.first = index;
	cmd.count = 1;
	cmd.top = (top - visible.top()) * scale;
	cmd.bottom = (bottom + 1 - visible.top()) * scale;
	cmd.width = visible.width() * scale;
	cmd.xoffs = visible.left() * scale;
	commands.push_back(cmd);
	presents++;
}


void midvunit_vk::impl::begin_pass(VkCommandBuffer cmd, VkRenderPass const *variants, VkFramebuffer framebuffer, bool &ready, int w, int h)
{
	VkClearValue clear{};
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = variants[ready ? 0 : 1];
	pass.framebuffer = framebuffer;
	pass.renderArea = { { 0, 0 }, { u32(w), u32(h) } };
	pass.clearValueCount = 1;
	pass.pClearValues = &clear;
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

bool midvunit_vk::impl::flush(int readback_width, int readback_height)
{
	bool const copy_out = (readback_width > 0) && (readback_height > 0);
	bool const textures = std::find(texture_dirty.begin(), texture_dirty.end(), true) != texture_dirty.end();
	if (commands.empty() && !copy_out && !textures)
		return true;

	int const slot_index = next_slot;
	slot &sl = slots[slot_index];
	next_slot = (next_slot + 1) % SLOTS;
	wait_slot(sl);

	// quad records, span records, span values, palettes, then the texture pages
	u32 const quad_base = 0;
	u32 const span_base = quad_base + u32(quads.size());
	u32 const value_base = span_base + u32(spans.size());
	u32 const palette_base = value_base + u32(values.size());
	u32 const total = palette_base + u32(palettes.size());
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
	std::copy(quads.begin(), quads.end(), out + quad_base);
	std::copy(spans.begin(), spans.end(), out + span_base);
	std::copy(values.begin(), values.end(), out + value_base);
	std::copy(palettes.begin(), palettes.end(), out + palette_base);
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

	// uploads, once the previous batches are done reading and copying
	VkMemoryBarrier reused_memory{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	reused_memory.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	reused_memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &reused_memory, 0, nullptr, 0, nullptr);
	VkBufferCopy const region{ 0, 0, bytes };
	vkCmdCopyBuffer(cmd, sl.host.handle, data.handle, 1, &region);
	if (!texture_copies.empty())
		vkCmdCopyBuffer(cmd, sl.host.handle, texture.handle, u32(texture_copies.size()), texture_copies.data());
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);

	// replay
	int current = -1;
	VkPipeline bound = VK_NULL_HANDLE;
	for (command const &c : commands)
	{
		if (c.type == CMD_PRESENT)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			current = -1;
			for (int page = 0; page < 2; page++)
				if (!page_ready[page])
				{
					begin_pass(cmd, page_passes, page_framebuffers[page], page_ready[page], width, height);
					vkCmdEndRenderPass(cmd);
				}
			begin_pass(cmd, display_passes, display_framebuffer, display_ready, display_width, display_height);
			VkRect2D const band{ { 0, c.top }, { u32(c.width), u32(c.bottom - c.top) } };
			vkCmdSetScissor(cmd, 0, 1, &band);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline);
			push_constants const push = { 1.0f / display_width, 1.0f / display_height, 0, palette_base + c.first * PENS, scale, c.page, c.xoffs };
			vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
			vkCmdDraw(cmd, 3, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			bound = VK_NULL_HANDLE;
			continue;
		}
		if (c.page != current)
		{
			if (current >= 0)
				vkCmdEndRenderPass(cmd);
			begin_pass(cmd, page_passes, page_framebuffers[c.page], page_ready[c.page], width, height);
			current = c.page;
			bound = VK_NULL_HANDLE;
		}
		VkPipeline const pipeline = (c.type == CMD_QUADS) ? quad_pipeline : span_pipeline;
		if (pipeline != bound)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound = pipeline;
		}
		u32 const base = (c.type == CMD_QUADS) ? (quad_base + c.first * QR_SIZE) : (span_base + c.first * SR_SIZE);
		push_constants const push = { 1.0f / width, 1.0f / height, base, value_base, scale, 0, 0 };
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd, c.count * 6, 1, 0, 0);
	}
	if (current >= 0)
		vkCmdEndRenderPass(cmd);

	// copy the display image out
	int rb = -1;
	if (copy_out)
	{
		// not the frame being shown, preferably not the prefetched one
		for (int pass = 0; (pass < 2) && (rb < 0); pass++)
			for (int i = 0; (i < READBACKS) && (rb < 0); i++)
			{
				int const index = (next_readback + i) % READBACKS;
				if ((index != shown) && (pass || (index != prefetched)))
					rb = index;
			}
		next_readback = (rb + 1) % READBACKS;
		readback[rb].shared = finishing;
		if (finishing)
		{
			share->record(cmd, *finishing, display.handle, VK_IMAGE_LAYOUT_GENERAL, 0, 0);
		}
		else
		{
			VkBufferImageCopy copy{};
			copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.imageExtent = { u32(readback_width), u32(readback_height), 1 };
			vkCmdCopyImageToBuffer(cmd, display.handle, VK_IMAGE_LAYOUT_GENERAL, readback[rb].host.handle, 1, &copy);
			VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
			host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			host.buffer = readback[rb].host.handle;
			host.size = VK_WHOLE_SIZE;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
		}
	}
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vkResetFences(device, 1, &sl.fence);
	bool const ok = vkQueueSubmit(queue, 1, &submit, sl.fence) == VK_SUCCESS;
	if (ok && copy_out && finishing)
	{
		share->submitted(*finishing, true);
		share->tag(*finishing) = slot_index;
	}
	sl.busy = ok;
	flushes++;
	commands.clear();
	quads.clear();
	spans.clear();
	values.clear();
	palettes.clear();
	texture_low = ~u32(0);
	texture_high = 0;
	if (!ok)
		return false;

	if (rb >= 0)
	{
		readback[rb].page = -1;
		readback[rb].slot = slot_index;
		readback[rb].ready = false;
		last_readback = rb;
	}
	return true;
}


void midvunit_vk::impl::complete(readback_entry &entry)
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


bool midvunit_vk::impl::fits(rectangle const &visible) const
{
	return (visible.left() >= 0) && (visible.right() < NATIVE_SIZE) && (visible.top() >= 0) && (visible.height() <= NATIVE_SIZE) && !visible.empty();
}


bool midvunit_vk::impl::matches(readback_entry const &entry, int page, rectangle const &visible, rgb_t const *pens) const
{
	return (entry.page == page) && (entry.version == versions[page]) && (entry.visible == visible) &&
			(entry.pens.size() == PENS) && !std::memcmp(entry.pens.data(), pens, PENS * 4);
}


void midvunit_vk::impl::set_entry(int index, int page, rectangle const &visible, rgb_t const *pens)
{
	readback_entry &entry = readback[index];
	entry.page = page;
	entry.version = versions[page];
	entry.visible = visible;
	entry.pens.resize(PENS);
	std::memcpy(entry.pens.data(), pens, PENS * 4);
}


bitmap_rgb32 *midvunit_vk::impl::show(int index, rectangle const &visible)
{
	readback_entry &entry = readback[index];
	if (entry.shared)
	{
		// its batch is waited for in frame_ready()
		shown = index;
		frames++;
		return &share->bitmap(*entry.shared);
	}

	// high-resolution screen pixel (x, y) is image pixel (x - left, y - top)
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

std::unique_ptr<midvunit_vk> midvunit_vk::create(int scale, u16 const *vram, u8 const *texture, std::string &error)
{
	auto state = std::make_unique<impl>();
	if (!state->init(error))
		return nullptr;
	state->scale = scale;
	state->width = NATIVE_SIZE * scale;
	state->height = NATIVE_SIZE * scale;
	state->vram = vram;
	state->texram = texture;
	if (!state->create_targets())
	{
		error = "video RAM allocation failed";
		return nullptr;
	}
	return std::unique_ptr<midvunit_vk>(new midvunit_vk(std::move(state)));
}


midvunit_vk::midvunit_vk(std::unique_ptr<impl> &&state)
	: m(std::move(state))
{
}


midvunit_vk::~midvunit_vk()
{
}


std::string const &midvunit_vk::device_name() const
{
	return m->name;
}


void midvunit_vk::quad(int page, vertex const *v, int mode, u16 pixdata, bool dither, u32 texbase, rectangle const &clip)
{
	impl &s = *m;
	s.check_resync();
	page &= 1;

	// render_polygon<4>: the scanlines covered
	float minx = v[0].x;
	float maxx = v[0].x;
	int minv = 0;
	int maxv = 0;
	for (int i = 1; i < 4; i++)
	{
		if (v[i].y < v[minv].y)
			minv = i;
		else if (v[i].y > v[maxv].y)
			maxv = i;
		minx = std::min(minx, v[i].x);
		maxx = std::max(maxx, v[i].x);
	}
	s32 const top = std::max(round_coordinate(v[minv].y), clip.top());
	s32 const bottom = std::min(round_coordinate(v[maxv].y), clip.bottom() + 1);
	if (bottom - top <= 0)
		return;

	// the edges forward and backward from the top vertex to the bottom one,
	// without the horizontal ones
	struct edge
	{
		vertex const *v1, *v2;
		float dxdy;
		float dpdy[2];
	};
	edge forward[3]{}, backward[3]{};
	int fcount = 0;
	int bcount = 0;
	for (int cur = minv; cur != maxv; cur = (cur + 1) & 3)
	{
		edge &e = forward[fcount];
		e.v1 = &v[cur];
		e.v2 = &v[(cur + 1) & 3];
		if (e.v1->y == e.v2->y)
			continue;
		float const ooy = 1.0f / (e.v2->y - e.v1->y);
		e.dxdy = (e.v2->x - e.v1->x) * ooy;
		e.dpdy[0] = (e.v2->u - e.v1->u) * ooy;
		e.dpdy[1] = (e.v2->v - e.v1->v) * ooy;
		fcount++;
	}
	for (int cur = minv; cur != maxv; cur = (cur + 3) & 3)
	{
		edge &e = backward[bcount];
		e.v1 = &v[cur];
		e.v2 = &v[(cur + 3) & 3];
		if (e.v1->y == e.v2->y)
			continue;
		float const ooy = 1.0f / (e.v2->y - e.v1->y);
		e.dxdy = (e.v2->x - e.v1->x) * ooy;
		e.dpdy[0] = (e.v2->u - e.v1->u) * ooy;
		e.dpdy[1] = (e.v2->v - e.v1->v) * ooy;
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

	// columns the extents may cover, with the stop pixel render_flat's
	// memset draws
	s32 const boxleft = std::max<s32>(s32(std::floor(minx)) - 1, clip.left());
	s32 const boxright = std::min<s32>(std::min<s32>(s32(std::ceil(maxx)) + 2, clip.right() + 2), s.width);
	if (boxright <= boxleft)
		return;

	u32 const index = u32(s.quads.size() / QR_SIZE);
	size_t const base = s.quads.size();
	s.quads.resize(base + QR_SIZE, 0);
	u32 *const r = &s.quads[base];
	bool const inclusive = (mode == MODE_FLAT) && (pixdata == 0) && !dither;
	r[QR_FLAGS] = u32(mode) | (dither ? QF_DITHER : 0) | (inclusive ? QF_INCLUSIVE : 0) | (u32(lcount) << 8) | (u32(rcount) << 12);
	r[QR_PIXDATA] = pixdata;
	r[QR_TEXBASE] = texbase;
	r[QR_TOP] = top;
	r[QR_BOTTOM] = bottom;
	r[QR_LEFT] = clip.left();
	r[QR_RIGHT] = clip.right() + 1;
	r[QR_BOXLEFT] = boxleft;
	r[QR_BOXRIGHT] = boxright;
	r[QR_MAXY] = float_bits(v[maxv].y);
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
			out[QE_P0] = float_bits(e.v1->u);
			out[QE_P1] = float_bits(e.v1->v);
			out[QE_DP0DY] = float_bits(e.dpdy[0]);
			out[QE_DP1DY] = float_bits(e.dpdy[1]);
		}
	}
	if (mode != MODE_FLAT)
	{
		s.texture_low = std::min(s.texture_low, texbase);
		s.texture_high = std::max(s.texture_high, texbase + TEXTURE_WINDOW);
	}
	s.quad_count++;
	s.add_command(CMD_QUADS, page, index);

	// send batches to the GPU as they fill, without waiting
	if (s.quads.size() >= BATCH_QUADS * QR_SIZE)
		s.flush(0, 0);
}


void midvunit_vk::write(int page, int x, int y, u16 data)
{
	impl &s = *m;
	s.check_resync();
	page &= 1;
	s.write_count++;

	// a write next to the previous one extends its span
	if (!s.commands.empty())
	{
		command const &last = s.commands.back();
		if ((last.type == CMD_SPANS) && (last.page == page))
		{
			u32 *const span = &s.spans[s.spans.size() - SR_SIZE];
			if ((span[SR_Y] == u32(y)) && (span[SR_X] + span[SR_COUNT] == u32(x)))
			{
				span[SR_COUNT]++;
				s.values.push_back(data);
				s.versions[page]++;
				return;
			}
		}
	}
	s.add_span(page, x, y, 1, &data);
	if (s.values.size() >= BATCH_VALUES)
		s.flush(0, 0);
}


void midvunit_vk::texture_written(offs_t offset)
{
	impl &s = *m;
	if (offset >= TEXTURE_BYTES)
		return;

	// the quads waiting to be sent read the texture RAM as it was
	if ((offset >= s.texture_low) && (offset < s.texture_high))
	{
		s.texture_flushes++;
		s.flush(0, 0);
	}
	s.texture_dirty[offset / TEXTURE_PAGE] = true;
}


void midvunit_vk::resync()
{
	impl &s = *m;
	s.resync_pending = true;
	std::fill(s.texture_dirty.begin(), s.texture_dirty.end(), true);
	s.versions[0]++;
	s.versions[1]++;
}


void midvunit_vk::prefetch(int page, rectangle const &visible, rgb_t const *pens)
{
	impl &s = *m;
	page &= 1;
	int const w = visible.width() * s.scale;
	int const h = visible.height() * s.scale;
	if (s.sharing || s.band_pending || !s.fits(visible) || (w > s.display_width) || (h > s.display_height))
		return;
	for (int index : { s.prefetched, s.shown })
		if ((index >= 0) && s.matches(s.readback[index], page, visible, pens))
			return;
	s.check_resync();
	s.add_present(page, visible.top(), visible.bottom(), visible, pens);
	if (!s.flush(w, h) || (s.last_readback < 0))
		return;
	s.set_entry(s.last_readback, page, visible, pens);
	s.prefetched = s.last_readback;
	s.prefetches++;
}


bool midvunit_vk::update(int page, rectangle const &cliprect, rectangle const &visible, rgb_t const *pens, bitmap_rgb32 *&frame)
{
	impl &s = *m;
	frame = nullptr;
	page &= 1;
	int const w = visible.width() * s.scale;
	int const h = visible.height() * s.scale;
	if (!s.fits(visible) || !s.ensure_display(w, h))
		return false;
	int const top = std::max(cliprect.top(), visible.top());
	int const bottom = std::min(cliprect.bottom(), visible.bottom());
	bool const last = cliprect.bottom() >= visible.bottom();

	// a whole frame: the page converted when it became visible, or the frame
	// shown last, if the page and the colours have not changed since
	if (!s.band_pending && (top == visible.top()) && last)
	{
		for (int index : { s.prefetched, s.shown })
		{
			if ((index >= 0) && s.matches(s.readback[index], page, visible, pens))
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

	// convert the rows now, in order with the drawing; the frame is read back
	// once its bottom is reached
	s.check_resync();
	if (top <= bottom)
		s.add_present(page, top, bottom, visible, pens);
	if (!last)
	{
		s.band_pending = true;
		return true;
	}
	bool const several = s.band_pending;
	s.band_pending = false;
	s.prefetched = -1;

	// a texture of the display, or a read-back buffer
	s32 const left = visible.left() * s.scale;
	s32 const first = visible.top() * s.scale;
	s.finishing = s.share->acquire(rectangle(left, left + w - 1, first, first + h - 1));
	s.sharing = s.finishing != nullptr;
	bool const flushed = s.flush(w, h);
	s.finishing = nullptr;
	if (!flushed || (s.last_readback < 0))
		return false;
	int const index = s.last_readback;
	if (several)
		s.readback[index].page = -2;
	else
		s.set_entry(index, page, visible, pens);
	frame = s.show(index, visible);
	return true;
}


void midvunit_vk::frame_ready()
{
	impl &s = *m;
	if ((s.shown < 0) || !s.readback[s.shown].shared)
		return;

	// a frame shown again needs no wait: the batch is only waited for once
	int &slot = s.share->tag(*s.readback[s.shown].shared);
	if (slot >= 0)
		s.wait_slot(s.slots[slot]);
	slot = -1;
}


hires3d_output::gpu_texture midvunit_vk::shared_texture(bitmap_rgb32 const *frame)
{
	impl &s = *m;
	vk3d_share::target *const shared = s.share->find(frame);
	return shared ? s.share->texture(*shared) : hires3d_output::gpu_texture();
}
