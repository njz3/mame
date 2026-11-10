// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    psxgpu_vk.cpp

    Vulkan renderer for the PlayStation GPU at a higher resolution (mame3d).
    See psxgpu_vk.h.

***************************************************************************/

#include "emu.h"
#include "psxgpu_vk.h"

#include "vk3d_context.h"
#include "vk3d_share.h"

#include <algorithm>
#include <cstring>
#include <vector>


namespace {

#include "psxgpu_vk_shaders/layout.h"

// shaders compiled with glslc from psxgpu_vk_shaders (see NOTES.md)
u32 const s_poly_vert[] = {
#include "psxgpu_vk_shaders/poly.vert.spv.inc"
};
u32 const s_poly_frag[] = {
#include "psxgpu_vk_shaders/poly.frag.spv.inc"
};

constexpr VkFormat COLOR_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr int VRAM_WIDTH = 1024;
constexpr int VRAM_HEIGHT = 1024;
constexpr u32 VRAM_WORDS = VRAM_WIDTH * VRAM_HEIGHT / 2;

enum pipeline_id { PIPE_OPAQUE, PIPE_HALF, PIPE_ADD, PIPE_SUB, PIPE_QUARTER, PIPE_COUNT };

struct push_constants
{
	float inv_width, inv_height, scale;
	s32 pass;
	u32 records;
};

enum command_type : u8 { CMD_DRAW, CMD_MOVE };

struct command
{
	command_type type;
	u8 pipeline;
	s8 pass;
	u32 first, count;       // triangles
	VkRect2D scissor;
	s32 sx, sy, dx, dy, w, h;
};

u32 float_bits(float value)
{
	u32 bits;
	std::memcpy(&bits, &value, 4);
	return bits;
}

} // anonymous namespace


struct psxgpu_vk::impl : vk3d_context
{
	int scale = 1;
	int width = 0;
	int height = 0;

	VkRenderPass pass_load = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipeline pipelines[PIPE_COUNT]{};

	image vram;
	image scratch;          // for the moves, which may overlap
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	bool vram_ready = false;

	image display;
	int display_width = 0;
	int display_height = 0;

	buffer data;
	buffer host;
	buffer readback;
	bool busy = false;
	bool failed = false;

	std::unique_ptr<vk3d_share> share;

	std::vector<u32> records;
	std::vector<command> commands;

	u64 triangles = 0;
	u32 frames = 0;
	u32 batches = 0;
	osd_ticks_t wait_ticks = 0;

	~impl();

	bool init(std::string &error);
	VkPipeline make_pipeline(int id);
	bool ensure_data(VkDeviceSize bytes);
	bool ensure_display(int w, int h);
	void wait();
	void add_triangles(u32 flags, u32 tex, u32 clut, u32 mode, psxgpu_vk::vertex const *v, int count, VkRect2D const &scissor, int pipeline, int pass);
	bool submit(u16 const *const *vram_rows, bool with_output);
	void record_batch(VkCommandBuffer cmd);
	void vram_barrier(VkCommandBuffer cmd);
};


psxgpu_vk::impl::~impl()
{
	if (frames)
		osd_printf_verbose("psxgpu_vk: %u frames, %u batches, %u triangles, %.1f ms waiting for the GPU\n", frames, batches, u32(triangles), wait_ticks * 1000.0 / osd_ticks_per_second());
	if (device && vkDestroyDevice)
	{
		vkDeviceWaitIdle(device);
		share.reset();
		if (framebuffer)
			vkDestroyFramebuffer(device, framebuffer, nullptr);
		destroy_image(vram);
		destroy_image(scratch);
		destroy_image(display);
		destroy_buffer(data);
		destroy_buffer(host);
		destroy_buffer(readback);
		for (VkPipeline p : pipelines)
			if (p)
				vkDestroyPipeline(device, p, nullptr);
		if (layout)
			vkDestroyPipelineLayout(device, layout, nullptr);
		if (pool)
			vkDestroyDescriptorPool(device, pool, nullptr);
		if (set_layout)
			vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		if (pass_load)
			vkDestroyRenderPass(device, pass_load, nullptr);
	}
}


bool psxgpu_vk::impl::init(std::string &error)
{
	if (!init_device(error))
		return false;
	share = std::make_unique<vk3d_share>(*this);
	width = VRAM_WIDTH * scale;
	height = VRAM_HEIGHT * scale;

	VkAttachmentDescription attachment{};
	attachment.format = COLOR_FORMAT;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
	attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	VkAttachmentReference color_ref{ 0, VK_IMAGE_LAYOUT_GENERAL };
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;
	VkSubpassDependency dependencies[2]{};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = 1;
	info.pAttachments = &attachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	if (vkCreateRenderPass(device, &info, nullptr, &pass_load) != VK_SUCCESS)
	{
		error = "vkCreateRenderPass failed";
		return false;
	}

	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo set_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	set_info.bindingCount = 1;
	set_info.pBindings = &binding;
	vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout);
	VkPushConstantRange push_range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants) };
	VkPipelineLayoutCreateInfo layout_info{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	vkCreatePipelineLayout(device, &layout_info, nullptr, &layout);
	VkDescriptorPoolSize const pool_size{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
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
	for (int i = 0; i < PIPE_COUNT; i++)
	{
		pipelines[i] = make_pipeline(i);
		if (!pipelines[i])
		{
			error = "pipeline creation failed";
			return false;
		}
	}

	VkImageUsageFlags const usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (!create_image(vram, width, height, COLOR_FORMAT, usage, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_image(scratch, width, height, COLOR_FORMAT, usage, VK_IMAGE_ASPECT_COLOR_BIT))
	{
		error = "VRAM allocation failed";
		return false;
	}
	VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fb.renderPass = pass_load;
	fb.attachmentCount = 1;
	fb.pAttachments = &vram.view;
	fb.width = width;
	fb.height = height;
	fb.layers = 1;
	if (vkCreateFramebuffer(device, &fb, nullptr, &framebuffer) != VK_SUCCESS)
	{
		error = "frame buffer creation failed";
		return false;
	}
	return ensure_data(1 << 23);
}


VkPipeline psxgpu_vk::impl::make_pipeline(int id)
{
	VkShaderModule const vertex_module = create_module(s_poly_vert, sizeof(s_poly_vert));
	VkShaderModule const fragment_module = create_module(s_poly_frag, sizeof(s_poly_frag));
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
	float constant = 0.0f;
	if (id != PIPE_OPAQUE)
	{
		blend.blendEnable = VK_TRUE;
		blend.colorBlendOp = (id == PIPE_SUB) ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		if (id == PIPE_HALF)
		{
			constant = 0.5f;
			blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
			blend.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
		}
		else if (id == PIPE_QUARTER)
		{
			constant = 0.25f;
			blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
		}
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo blending{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blending.attachmentCount = 1;
	blending.pAttachments = &blend;
	blending.blendConstants[0] = blending.blendConstants[1] = blending.blendConstants[2] = blending.blendConstants[3] = constant;
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
	info.renderPass = pass_load;
	VkPipeline result = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return result;
}


bool psxgpu_vk::impl::ensure_data(VkDeviceSize bytes)
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
	VkDescriptorBufferInfo info{ data.handle, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &info;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	return true;
}


bool psxgpu_vk::impl::ensure_display(int w, int h)
{
	if ((w == display_width) && (h == display_height))
		return true;
	wait();
	destroy_image(display);
	destroy_buffer(readback);
	display_width = display_height = 0;
	if (!create_image(display, w, h, COLOR_FORMAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
			!create_buffer(readback, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
		return false;
	display_width = w;
	display_height = h;
	return true;
}


void psxgpu_vk::impl::wait()
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


void psxgpu_vk::impl::add_triangles(u32 flags, u32 tex, u32 clut, u32 mode, psxgpu_vk::vertex const *v, int count, VkRect2D const &scissor, int pipeline, int pass)
{
	u32 const first = u32(records.size() / TR_SIZE);
	for (int t = 0; t < count; t++)
	{
		size_t const base = records.size();
		records.resize(base + TR_SIZE);
		u32 *const r = &records[base];
		r[TR_FLAGS] = flags;
		r[TR_TEX] = tex;
		r[TR_CLUT] = clut;
		r[TR_MODE] = mode;
		for (int i = 0; i < 3; i++)
		{
			psxgpu_vk::vertex const &a = v[t * 3 + i];
			u32 *const o = &r[TR_VERTS + i * TV_SIZE];
			o[TV_X] = float_bits(a.x);
			o[TV_Y] = float_bits(a.y);
			o[TV_R] = float_bits(a.r);
			o[TV_G] = float_bits(a.g);
			o[TV_B] = float_bits(a.b);
			o[TV_U] = float_bits(a.u);
			o[TV_V] = float_bits(a.v);
		}
	}
	triangles += count;
	if (!commands.empty())
	{
		command &last = commands.back();
		if ((last.type == CMD_DRAW) && (last.pipeline == pipeline) && (last.pass == pass) && (last.first + last.count == first) && !std::memcmp(&last.scissor, &scissor, sizeof(VkRect2D)))
		{
			last.count += count;
			return;
		}
	}
	command cmd{};
	cmd.type = CMD_DRAW;
	cmd.pipeline = u8(pipeline);
	cmd.pass = s8(pass);
	cmd.first = first;
	cmd.count = u32(count);
	cmd.scissor = scissor;
	commands.push_back(cmd);
}


void psxgpu_vk::impl::vram_barrier(VkCommandBuffer cmd)
{
	VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}


void psxgpu_vk::impl::record_batch(VkCommandBuffer cmd)
{
	// the VRAM image exists from the start, cleared to black
	if (!vram_ready)
	{
		VkImageMemoryBarrier to_general{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_general.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier barriers[2] = { to_general, to_general };
		barriers[0].image = vram.handle;
		barriers[1].image = scratch.handle;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
		VkClearColorValue const black{};
		VkImageSubresourceRange const range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, vram.handle, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
		vram_ready = true;
	}

	push_constants push{};
	push.inv_width = 1.0f / width;
	push.inv_height = 1.0f / height;
	push.scale = float(scale);
	push.records = VRAM_WORDS;

	VkViewport const viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
	VkRect2D const whole{ { 0, 0 }, { u32(width), u32(height) } };
	VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	pass.renderPass = pass_load;
	pass.framebuffer = framebuffer;
	pass.renderArea = whole;

	bool open = false;
	int bound = -1;
	for (command const &c : commands)
	{
		if (c.type == CMD_MOVE)
		{
			if (open)
				vkCmdEndRenderPass(cmd);
			open = false;
			vram_barrier(cmd);
			VkImageCopy copy{};
			copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			copy.dstSubresource = copy.srcSubresource;
			copy.srcOffset = { c.sx, c.sy, 0 };
			copy.dstOffset = { c.dx, c.dy, 0 };
			copy.extent = { u32(c.w), u32(c.h), 1 };
			VkImageCopy to_scratch = copy;
			to_scratch.dstOffset = copy.srcOffset;
			vkCmdCopyImage(cmd, vram.handle, VK_IMAGE_LAYOUT_GENERAL, scratch.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &to_scratch);
			vram_barrier(cmd);
			vkCmdCopyImage(cmd, scratch.handle, VK_IMAGE_LAYOUT_GENERAL, vram.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
			vram_barrier(cmd);
			continue;
		}
		if (!open)
		{
			vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
			open = true;
			bound = -1;
		}
		if (c.pipeline != bound)
		{
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[c.pipeline]);
			bound = c.pipeline;
		}
		vkCmdSetScissor(cmd, 0, 1, &c.scissor);
		push.pass = c.pass;
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
		vkCmdDraw(cmd, c.count * 3, 1, c.first * 3, 0);
	}
	if (open)
		vkCmdEndRenderPass(cmd);
}


bool psxgpu_vk::impl::submit(u16 const *const *vram_rows, bool with_output)
{
	if (commands.empty() && !with_output)
		return true;
	wait();
	u32 const record_words = u32(records.size());
	VkDeviceSize const bytes = VkDeviceSize(VRAM_WORDS + record_words) * 4;
	if (failed || !ensure_data(bytes))
	{
		failed = true;
		return false;
	}
	u32 *const out = reinterpret_cast<u32 *>(host.mapped);
	for (int y = 0; y < VRAM_HEIGHT; y++)
		std::memcpy(reinterpret_cast<u16 *>(out) + size_t(y) * VRAM_WIDTH, vram_rows[y], VRAM_WIDTH * 2);
	if (record_words)
		std::memcpy(out + VRAM_WORDS, records.data(), record_words * 4);
	flush_buffer(host, 0, bytes);

	VkCommandBuffer const cmd = command_buffer;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);
	VkBufferCopy const region{ 0, 0, bytes };
	vkCmdCopyBuffer(cmd, host.handle, data.handle, 1, &region);
	VkMemoryBarrier uploaded{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	uploaded.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	uploaded.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &uploaded, 0, nullptr, 0, nullptr);
	record_batch(cmd);
	commands.clear();
	records.clear();
	batches++;
	return true;
}


psxgpu_vk::psxgpu_vk(std::unique_ptr<impl> &&state) : m(std::move(state))
{
}


psxgpu_vk::~psxgpu_vk()
{
}


std::unique_ptr<psxgpu_vk> psxgpu_vk::create(int scale, std::string &error)
{
	auto state = std::make_unique<impl>();
	state->scale = scale;
	if (!state->init(error))
		return nullptr;
	return std::unique_ptr<psxgpu_vk>(new psxgpu_vk(std::move(state)));
}


std::string const &psxgpu_vk::device_name() const
{
	return m->name;
}


bool psxgpu_vk::failed() const
{
	return m->failed;
}


void psxgpu_vk::draw(primitive const &p, vertex const *v, int count)
{
	impl &s = *m;
	if (s.failed)
		return;
	int const left = std::clamp(p.clip_left, 0, VRAM_WIDTH) * s.scale;
	int const top = std::clamp(p.clip_top, 0, VRAM_HEIGHT) * s.scale;
	int const right = std::clamp(p.clip_right + 1, 0, VRAM_WIDTH) * s.scale;
	int const bottom = std::clamp(p.clip_bottom + 1, 0, VRAM_HEIGHT) * s.scale;
	if ((right <= left) || (bottom <= top) || !count)
		return;
	VkRect2D const scissor{ { left, top }, { u32(right - left), u32(bottom - top) } };
	u32 const flags = (p.textured ? TF_TEXTURED : 0) | (p.raw ? TF_RAW : 0) | (p.interleaved ? TF_INTERLEAVED : 0);
	u32 const tex = u32(p.tx) | (u32(p.ty) << 16);
	u32 const clut = u32(p.clut_x) | (u32(p.clut_y) << 16);
	u32 const mode = u32(p.tp) | (u32(p.tww) << 8) | (u32(p.twh) << 16);
	int const semi_pipe = (p.abr == 0) ? PIPE_HALF : (p.abr == 1) ? PIPE_ADD : (p.abr == 2) ? PIPE_SUB : PIPE_QUARTER;
	if (p.textured && p.semi)
	{
		// the texels without bit 15 are opaque
		s.add_triangles(flags, tex, clut, mode, v, count, scissor, PIPE_OPAQUE, PASS_OPAQUE);
		s.add_triangles(flags, tex, clut, mode, v, count, scissor, semi_pipe, PASS_SEMI);
	}
	else if (p.semi)
	{
		s.add_triangles(flags, tex, clut, mode, v, count, scissor, semi_pipe, PASS_ALL);
	}
	else
	{
		s.add_triangles(flags, tex, clut, mode, v, count, scissor, PIPE_OPAQUE, PASS_ALL);
	}
}


void psxgpu_vk::fill(int x, int y, int w, int h, rgb_t color)
{
	impl &s = *m;
	if (s.failed)
		return;
	float const x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
	float const r = color.r(), g = color.g(), b = color.b();
	vertex const v[6] = {
		{ x0, y0, r, g, b, 0, 0 }, { x1, y0, r, g, b, 0, 0 }, { x1, y1, r, g, b, 0, 0 },
		{ x0, y0, r, g, b, 0, 0 }, { x1, y1, r, g, b, 0, 0 }, { x0, y1, r, g, b, 0, 0 } };
	VkRect2D const scissor{ { 0, 0 }, { u32(s.width), u32(s.height) } };
	s.add_triangles(0, 0, 0, 0, v, 2, scissor, PIPE_OPAQUE, PASS_ALL);
}


void psxgpu_vk::flush(u16 const *const *vram)
{
	impl &s = *m;
	if (s.commands.empty())
		return;
	if (!s.submit(vram, false))
		return;
	VkCommandBuffer const cmd = s.command_buffer;
	s.vkEndCommandBuffer(cmd);
	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	if (s.vkQueueSubmit(s.queue, 1, &submit, s.fence) == VK_SUCCESS)
		s.busy = true;
	else
		s.failed = true;
}


void psxgpu_vk::upload(int x, int y, int w, int h)
{
	impl &s = *m;
	if (s.failed)
		return;
	float const x0 = float(x), y0 = float(y), x1 = float(x + w), y1 = float(y + h);
	vertex const v[6] = {
		{ x0, y0, 0, 0, 0, x0, y0 }, { x1, y0, 0, 0, 0, x1, y0 }, { x1, y1, 0, 0, 0, x1, y1 },
		{ x0, y0, 0, 0, 0, x0, y0 }, { x1, y1, 0, 0, 0, x1, y1 }, { x0, y1, 0, 0, 0, x0, y1 } };
	VkRect2D const scissor{ { 0, 0 }, { u32(s.width), u32(s.height) } };
	s.add_triangles(TF_TEXTURED | TF_RAW | TF_DIRECT, 0, 0, 0, v, 2, scissor, PIPE_OPAQUE, PASS_ALL);
}


void psxgpu_vk::move(int sx, int sy, int dx, int dy, int w, int h)
{
	impl &s = *m;
	if (s.failed || (w <= 0) || (h <= 0) || (sx < 0) || (sy < 0) || (dx < 0) || (dy < 0) || (sx + w > VRAM_WIDTH) || (dx + w > VRAM_WIDTH) || (sy + h > VRAM_HEIGHT) || (dy + h > VRAM_HEIGHT))
		return;
	command c{};
	c.type = CMD_MOVE;
	c.sx = sx * s.scale;
	c.sy = sy * s.scale;
	c.dx = dx * s.scale;
	c.dy = dy * s.scale;
	c.w = w * s.scale;
	c.h = h * s.scale;
	s.commands.push_back(c);
}


bool psxgpu_vk::show(u16 const *const *vram, int x, int y, int w, int h, int left, int top, int screen_w, int screen_h, hires3d_output &output, rectangle const &shown)
{
	impl &s = *m;
	int const frame_w = screen_w * s.scale;
	int const frame_h = screen_h * s.scale;
	if (!s.ensure_display(frame_w, frame_h) || !s.submit(vram, true))
		return false;
	VkCommandBuffer const cmd = s.command_buffer;

	// the area shown, in a black frame
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = s.display.handle;
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	s.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkClearColorValue const black{};
	VkImageSubresourceRange const range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	s.vkCmdClearColorImage(cmd, s.display.handle, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
	s.vram_barrier(cmd);
	if ((w > 0) && (h > 0) && (x >= 0) && (y >= 0) && (x + w <= VRAM_WIDTH) && (y + h <= VRAM_HEIGHT))
	{
		VkImageCopy copy{};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = copy.srcSubresource;
		copy.srcOffset = { x * s.scale, y * s.scale, 0 };
		copy.dstOffset = { left * s.scale, top * s.scale, 0 };
		copy.extent = { u32(w * s.scale), u32(h * s.scale), 1 };
		s.vkCmdCopyImage(cmd, s.vram.handle, VK_IMAGE_LAYOUT_GENERAL, s.display.handle, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	}
	s.vram_barrier(cmd);

	vk3d_share::target *const shared = s.share->acquire(shown);
	if (shared)
	{
		s.share->record(cmd, *shared, s.display.handle, VK_IMAGE_LAYOUT_GENERAL, shown.left(), shown.top());
	}
	else
	{
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { u32(frame_w), u32(frame_h), 1 };
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
	{
		s.failed = true;
		return false;
	}
	s.busy = true;
	s.frames++;

	if (shared)
	{
		s.share->submitted(*shared, true);
		output.use_frame(s.share->bitmap(*shared), [this] () { frame_ready(); }, s.share->texture(*shared));
		return true;
	}

	s.wait();
	if (s.failed)
		return false;
	if (!s.readback.coherent)
	{
		VkMappedMemoryRange mapped{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		mapped.memory = s.readback.memory;
		mapped.size = VK_WHOLE_SIZE;
		s.vkInvalidateMappedMemoryRanges(s.device, 1, &mapped);
	}
	bitmap_rgb32 &frame = output.frame_bitmap();
	rectangle area = shown;
	area &= rectangle(0, std::min(frame_w, frame.width()) - 1, 0, std::min(frame_h, frame.height()) - 1);
	u32 const *const base = reinterpret_cast<u32 const *>(s.readback.mapped);
	for (int line = area.top(); line <= area.bottom(); line++)
		std::memcpy(&frame.pix(line, area.left()), base + size_t(line) * frame_w + area.left(), size_t(area.width()) * 4);
	return true;
}


void psxgpu_vk::frame_ready()
{
	m->wait();
}
