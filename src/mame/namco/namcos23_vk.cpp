// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    namcos23_vk.cpp

    Vulkan renderer for the Namco System 23 / Super System 23 3D at a higher
    resolution (mame3d).  See namcos23_vk.h.

    A frame is recorded on the CPU and sent in one batch.  It is handed to
    a texture of the display when it can draw one, or read back.

***************************************************************************/

#include "emu.h"
#include "namcos23_vk.h"

#include "vk3d_context.h"
#include "vk3d_share.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

#include "namcos23_vk_shaders/layout.h"

// shaders compiled with glslc from namcos23_vk_shaders (see NOTES.md)
u32 const s_poly_vert[] = {
#include "namcos23_vk_shaders/poly.vert.spv.inc"
};
u32 const s_poly_frag[] = {
#include "namcos23_vk_shaders/poly.frag.spv.inc"
};
u32 const s_mix_vert[] = {
#include "namcos23_vk_shaders/mix.vert.spv.inc"
};
u32 const s_mix_frag[] = {
#include "namcos23_vk_shaders/mix.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr u32 NATIVE_WIDTH = 640;
constexpr u32 NATIVE_HEIGHT = 480;
constexpr u32 STENCIL_WORDS = 0x10000;
constexpr u32 GAMMA_WORDS = 0x400;

struct push_constants
{
	float inv_width, inv_height;
	u32 palette, stencil, text, gamma;
	s32 scale, pad, frame_width, stretch2d;
	u32 tileid_mask;
	s32 text_fade, fade_factor, fade_factor_inv;
	u32 fade_rgb;
	s32 alpha_factor, alpha_factor_inv;
	u32 alpha_mask, alpha_low, alpha_high;
};

u32 float_bits(float value)
{
	u32 bits;
	std::memcpy(&bits, &value, 4);
	return bits;
}

} // anonymous namespace


struct namcos23_vk::impl : vk3d_context
{
	int scale = 1;
	int pad = 0;
	bool stretch2d = false;
	int width = 0;
	int height = 0;
	u32 tileid_mask = 0;

	VkRenderPass scene_pass = VK_NULL_HANDLE;
	VkRenderPass display_pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkPipeline poly_pipeline = VK_NULL_HANDLE;
	VkPipeline mix_pipeline = VK_NULL_HANDLE;

	image scene;
	image display;
	VkFramebuffer scene_framebuffer = VK_NULL_HANDLE;
	VkFramebuffer display_framebuffer = VK_NULL_HANDLE;

	buffer texels;
	buffer tiles;
	buffer data;
	buffer host;
	buffer readback;
	bool busy = false;
	bool failed = false;      // the GPU stopped working

	std::unique_ptr<vk3d_share> share;
	vk3d_share::target *shown_shared = nullptr;

	std::vector<u32> records;
	VkClearValue clear{};

	// statistics, shown with -verbose
	u64 triangles = 0;
	u32 frames = 0;
	osd_ticks_t wait_ticks = 0;

	~impl();

	bool init(int w, int h, u8 const *texel_data, size_t texel_bytes, u32 const *tile_data, u32 tile_count, std::string &error);
	VkPipeline make_pipeline(u32 const *vertex, size_t vertex_size, u32 const *fragment, size_t fragment_size, VkRenderPass pass, bool blend);
	bool ensure_data(VkDeviceSize bytes);
	void wait();
};


namcos23_vk::impl::~impl()
{
	if (frames)
		osd_printf_verbose("namcos23_vk: %u frames, %u triangles, %.1f ms waiting for the GPU\n", frames, u32(triangles), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		share.reset();
		if (scene_framebuffer)
			vkDestroyFramebuffer(device, scene_framebuffer, nullptr);
		if (display_framebuffer)
			vkDestroyFramebuffer(device, display_framebuffer, nullptr);
		destroy_image(scene);
		destroy_image(display);
		destroy_buffer(texels);
		destroy_buffer(tiles);
		destroy_buffer(data);
		destroy_buffer(host);
		destroy_buffer(readback);
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
		if (scene_pass)
			vkDestroyRenderPass(device, scene_pass, nullptr);
		if (display_pass)
			vkDestroyRenderPass(device, display_pass, nullptr);
	}
}


bool namcos23_vk::impl::init(int w, int h, u8 const *texel_data, size_t texel_bytes, u32 const *tile_data, u32 tile_count, std::string &error)
{
	if (!init_device(error))
		return false;
	share = std::make_unique<vk3d_share>(*this);
	width = w;
	height = h;

	// render passes: the scene is read by the mix, the display copied out
	for (int target = 0; target < 2; target++)
	{
		VkAttachmentDescription attachment{};
		attachment.format = COLOR_FORMAT;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = target ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
		VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_GENERAL };
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color_ref;
		VkSubpassDependency dependencies[2]{};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 1;
		info.pAttachments = &attachment;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;
		info.dependencyCount = 2;
		info.pDependencies = dependencies;
		if (vkCreateRenderPass(device, &info, nullptr, target ? &display_pass : &scene_pass) != VK_SUCCESS)
		{
			error = "vkCreateRenderPass failed";
			return false;
		}
	}

	VkDescriptorSetLayoutBinding bindings[4]{};
	for (int i = 0; i < 4; i++)
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = (i < 3) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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
	VkDescriptorPoolSize const pool_sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
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

	poly_pipeline = make_pipeline(s_poly_vert, sizeof(s_poly_vert), s_poly_frag, sizeof(s_poly_frag), scene_pass, true);
	mix_pipeline = make_pipeline(s_mix_vert, sizeof(s_mix_vert), s_mix_frag, sizeof(s_mix_frag), display_pass, false);
	if (!poly_pipeline || !mix_pipeline)
	{
		error = "pipeline creation failed";
		return false;
	}

	// frame buffers
	if (!create_image(scene, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_image(display, width, height, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_buffer(readback, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
	{
		error = "frame buffer allocation failed";
		return false;
	}
	for (int target = 0; target < 2; target++)
	{
		VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		info.renderPass = target ? display_pass : scene_pass;
		info.attachmentCount = 1;
		info.pAttachments = target ? &display.view : &scene.view;
		info.width = width;
		info.height = height;
		info.layers = 1;
		if (vkCreateFramebuffer(device, &info, nullptr, target ? &display_framebuffer : &scene_framebuffer) != VK_SUCCESS)
		{
			error = "frame buffer creation failed";
			return false;
		}
	}

	// texture ROM and tile map, uploaded once
	VkDeviceSize const tile_bytes = VkDeviceSize(tile_count) * 4;
	buffer staging;
	if (!create_device_buffer(texels, texel_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
			!create_device_buffer(tiles, tile_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
			!create_buffer(staging, texel_bytes + tile_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
	{
		error = "texture memory allocation failed";
		return false;
	}
	std::memcpy(staging.mapped, texel_data, texel_bytes);
	std::memcpy(reinterpret_cast<u8 *>(staging.mapped) + texel_bytes, tile_data, tile_bytes);
	flush_buffer(staging, 0, texel_bytes + tile_bytes);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(command_buffer, 0);
	vkBeginCommandBuffer(command_buffer, &begin);
	VkBufferCopy region{ 0, 0, texel_bytes };
	vkCmdCopyBuffer(command_buffer, staging.handle, texels.handle, 1, &region);
	region = { texel_bytes, 0, tile_bytes };
	vkCmdCopyBuffer(command_buffer, staging.handle, tiles.handle, 1, &region);
	vkEndCommandBuffer(command_buffer);
	bool const uploaded = submit_and_wait();
	destroy_buffer(staging);
	if (!uploaded)
	{
		error = "texture upload failed";
		return false;
	}
	return ensure_data(1 << 24);
}


bool namcos23_vk::impl::ensure_data(VkDeviceSize bytes)
{
	if (data.size >= bytes)
		return true;
	wait();
	destroy_buffer(data);
	destroy_buffer(host);
	VkDeviceSize const size = bytes * 3 / 2;
	if (!create_device_buffer(data, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
			!create_buffer(host, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false))
		return false;

	buffer const *const sources[3] = { &texels, &tiles, &data };
	VkDescriptorBufferInfo infos[3];
	VkDescriptorImageInfo image_info{ sampler, scene.view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet writes[4]{};
	for (int i = 0; i < 4; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		if (i < 3)
		{
			infos[i] = { sources[i]->handle, 0, VK_WHOLE_SIZE };
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writes[i].pBufferInfo = &infos[i];
		}
		else
		{
			writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[i].pImageInfo = &image_info;
		}
	}
	vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
	return true;
}


VkPipeline namcos23_vk::impl::make_pipeline(u32 const *vertex, size_t vertex_size, u32 const *fragment, size_t fragment_size, VkRenderPass pass, bool blend_on)
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
	if (blend_on)
	{
		blend.blendEnable = VK_TRUE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
	info.pColorBlendState = &blending;
	info.pDynamicState = &dynamic;
	info.layout = layout;
	info.renderPass = pass;
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return result;
}


void namcos23_vk::impl::wait()
{
	if (!busy)
		return;
	osd_ticks_t const start = osd_ticks();
	if (vkWaitForFences(device, 1, &fence, VK_TRUE, ~u64(0)) != VK_SUCCESS)
		failed = true;
	vkResetFences(device, 1, &fence);
	wait_ticks += osd_ticks() - start;
	busy = false;
}


namcos23_vk::namcos23_vk(std::unique_ptr<impl> &&state) : m(std::move(state))
{
}


namcos23_vk::~namcos23_vk()
{
}


std::unique_ptr<namcos23_vk> namcos23_vk::create(int scale, int pad, bool stretch2d, u8 const *texels, size_t texel_bytes, u32 const *tiles, u32 tile_count, u32 tileid_mask, std::string &error)
{
	auto state = std::make_unique<impl>();
	state->scale = scale;
	state->pad = pad;
	state->stretch2d = stretch2d;
	state->tileid_mask = tileid_mask;
	if (!state->init(NATIVE_WIDTH * scale + 2 * pad, NATIVE_HEIGHT * scale, texels, texel_bytes, tiles, tile_count, error))
		return nullptr;
	return std::unique_ptr<namcos23_vk>(new namcos23_vk(std::move(state)));
}


std::string const &namcos23_vk::device_name() const
{
	return m->name;
}


void namcos23_vk::begin_frame(rgb_t background)
{
	m->records.clear();
	m->clear.color.float32[0] = background.b() / 255.0f;
	m->clear.color.float32[1] = background.g() / 255.0f;
	m->clear.color.float32[2] = background.r() / 255.0f;
	m->clear.color.float32[3] = 1.0f;
}


void namcos23_vk::add_polygon(polygon const &p, vertex const *v, int count)
{
	impl &s = *m;
	u32 const flags = (p.stencil ? TF_STENCIL : 0) | (p.shade ? TF_SHADE : 0) | (p.poly_fade ? TF_POLY_FADE : 0) | (p.color_fade ? TF_COLOR_FADE : 0) |
			(p.blend ? TF_BLEND : 0) | (p.poly_alpha ? TF_POLY_ALPHA : 0) | (p.alpha_enabled ? TF_ALPHA_ON : 0);
	float const scale = float(s.scale);
	float const left = float(p.clip_left) * scale + float(s.pad);
	float const top = float(p.clip_top) * scale;
	for (int t = 1; t + 1 < count; t++)
	{
		int const indices[3] = { 0, t, t + 1 };
		size_t const base = s.records.size();
		s.records.resize(base + TR_SIZE);
		u32 *const r = &s.records[base];
		r[TR_FLAGS] = flags;
		r[TR_TBASE] = p.tbase;
		r[TR_CMODE] = p.cmode;
		r[TR_PALETTE] = p.palette;
		r[TR_IBASE] = float_bits(p.intensity_base);
		r[TR_FADE] = u32(p.fade_factor) | (u32(p.fade_factor_inv) << 16);
		r[TR_FADE_RGB] = u32(p.fade_r) | (u32(p.fade_g) << 8) | (u32(p.fade_b) << 16);
		r[TR_POLY_RGB] = u32(p.poly_r) | (u32(p.poly_g) << 8) | (u32(p.poly_b) << 16);
		r[TR_ALPHA] = u32(p.alpha) | (u32(p.alpha_pen) << 16);
		r[TR_CLIP_LT] = u32(std::max(p.clip_left, 0)) | (u32(std::max(p.clip_top, 0)) << 16);
		r[TR_CLIP_RB] = u32(std::max(p.clip_right, 0)) | (u32(std::max(p.clip_bottom, 0)) << 16);
		for (int i = 0; i < 3; i++)
		{
			vertex const &a = v[indices[i]];
			u32 *const o = &r[TR_VERTS + i * TV_SIZE];
			o[TV_X] = float_bits(a.x * scale + left);
			o[TV_Y] = float_bits(a.y * scale + top);
			o[TV_P] = float_bits(a.z);
			o[TV_P + 1] = float_bits(a.u);
			o[TV_P + 2] = float_bits(a.v);
			o[TV_P + 3] = float_bits(a.i);
		}
		s.triangles++;
	}
}


bool namcos23_vk::end_frame(pen_t const *palette, u32 colors, u16 const *text, text_mix const &mix, u16 const *gamma, u16 const *stencil, hires3d_output &output, rectangle const &shown)
{
	impl &s = *m;
	s.wait();
	if (s.failed)
		return false;

	u32 const record_words = u32(s.records.size());
	u32 const palette_base = record_words;
	u32 const stencil_base = palette_base + colors;
	u32 const text_base = stencil_base + STENCIL_WORDS;
	u32 const gamma_base = text_base + NATIVE_WIDTH * NATIVE_HEIGHT;
	u32 const total = gamma_base + GAMMA_WORDS;
	VkDeviceSize const bytes = VkDeviceSize(total) * 4;
	if (!s.ensure_data(bytes))
		return false;

	u32 *const out = reinterpret_cast<u32 *>(s.host.mapped);
	if (record_words)
		std::memcpy(out, s.records.data(), record_words * 4);
	std::memcpy(out + palette_base, palette, colors * 4);
	for (u32 i = 0; i < STENCIL_WORDS; i++)
		out[stencil_base + i] = u32(stencil[i * 2]) | (u32(stencil[i * 2 + 1]) << 16);
	for (u32 i = 0; i < NATIVE_WIDTH * NATIVE_HEIGHT; i++)
		out[text_base + i] = text[i];
	for (u32 i = 0; i < GAMMA_WORDS; i++)
		out[gamma_base + i] = gamma[i];
	s.flush_buffer(s.host, 0, bytes);

	vk3d_share::target *const shared = s.share->acquire(shown);

	VkCommandBuffer const cmd = s.command_buffer;
	s.vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	s.vkBeginCommandBuffer(cmd, &begin);

	VkBufferCopy const region{ 0, 0, bytes };
	s.vkCmdCopyBuffer(cmd, s.host.handle, s.data.handle, 1, &region);
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);

	push_constants push{};
	push.inv_width = 1.0f / s.width;
	push.inv_height = 1.0f / s.height;
	push.palette = palette_base;
	push.stencil = stencil_base;
	push.text = text_base;
	push.gamma = gamma_base;
	push.scale = s.scale;
	push.pad = s.pad;
	push.frame_width = s.width;
	push.stretch2d = s.stretch2d ? 1 : 0;
	push.tileid_mask = s.tileid_mask;
	push.text_fade = mix.fade ? 1 : 0;
	push.fade_factor = mix.fade_factor;
	push.fade_factor_inv = mix.fade_factor_inv;
	push.fade_rgb = u32(mix.fade_r) | (u32(mix.fade_g) << 8) | (u32(mix.fade_b) << 16);
	push.alpha_factor = mix.alpha_factor;
	push.alpha_factor_inv = mix.alpha_factor_inv;
	push.alpha_mask = mix.alpha_mask;
	push.alpha_low = mix.alpha_low;
	push.alpha_high = mix.alpha_high;

	VkViewport const viewport{ 0.0f, 0.0f, float(s.width), float(s.height), 0.0f, 1.0f };
	VkRect2D const scissor{ { 0, 0 }, { u32(s.width), u32(s.height) } };

	// polygons
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = s.scene_pass;
	pass.framebuffer = s.scene_framebuffer;
	pass.renderArea = scissor;
	pass.clearValueCount = 1;
	pass.pClearValues = &s.clear;
	s.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	s.vkCmdSetViewport(cmd, 0, 1, &viewport);
	s.vkCmdSetScissor(cmd, 0, 1, &scissor);
	s.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &s.set, 0, nullptr);
	s.vkCmdPushConstants(cmd, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	if (record_words)
	{
		s.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.poly_pipeline);
		s.vkCmdDraw(cmd, (record_words / TR_SIZE) * 3, 1, 0, 0);
	}
	s.vkCmdEndRenderPass(cmd);

	// text and gamma
	pass.renderPass = s.display_pass;
	pass.framebuffer = s.display_framebuffer;
	pass.clearValueCount = 0;
	pass.pClearValues = nullptr;
	s.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
	s.vkCmdSetViewport(cmd, 0, 1, &viewport);
	s.vkCmdSetScissor(cmd, 0, 1, &scissor);
	s.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.mix_pipeline);
	s.vkCmdDraw(cmd, 3, 1, 0, 0);
	s.vkCmdEndRenderPass(cmd);

	// out
	if (shared)
	{
		s.share->record(cmd, *shared, s.display.handle, VK_IMAGE_LAYOUT_GENERAL, shown.left(), shown.top());
	}
	else
	{
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { u32(s.width), u32(s.height), 1 };
		s.vkCmdCopyImageToBuffer(cmd, s.display.handle, VK_IMAGE_LAYOUT_GENERAL, s.readback.handle, 1, &copy);
		VkMemoryBarrier host_read{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		host_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host_read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, nullptr, 0, nullptr);
	}
	s.vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	if (s.vkQueueSubmit(s.queue, 1, &submit, s.fence) != VK_SUCCESS)
		return false;
	s.busy = true;
	s.frames++;

	if (shared)
	{
		s.share->submitted(*shared, true);
		s.shown_shared = shared;
		output.use_frame(s.share->bitmap(*shared), [this] () { frame_ready(); }, s.share->texture(*shared));
		return true;
	}

	// read back now: the frame is in the output's own bitmap
	s.wait();
	if (s.failed)
		return false;
	if (!s.readback.coherent)
	{
		VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		range.memory = s.readback.memory;
		range.size = VK_WHOLE_SIZE;
		s.vkInvalidateMappedMemoryRanges(s.device, 1, &range);
	}
	bitmap_rgb32 &frame = output.frame_bitmap();
	rectangle area = shown;
	area &= rectangle(0, std::min(s.width, frame.width()) - 1, 0, std::min(s.height, frame.height()) - 1);
	u32 const *const base = reinterpret_cast<u32 const *>(s.readback.mapped);
	for (int y = area.top(); y <= area.bottom(); y++)
		std::memcpy(&frame.pix(y, area.left()), base + size_t(y) * s.width + area.left(), size_t(area.width()) * 4);
	return true;
}


void namcos23_vk::frame_ready()
{
	m->wait();
}
