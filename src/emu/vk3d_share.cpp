// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    vk3d_share.cpp

    Frames of a GPU renderer handed to the OSD renderer (mame3d).

    The textures come from the sink as Direct3D 11 textures shared
    through NT handles, imported as dedicated Vulkan memory.  Ownership
    goes to the external queue family once a frame is copied, and comes
    back when the next one is; the OSD renderer draws a texture after the
    renderer waited for its batch (hires3d ready callback), and acquire()
    waits for the draws of the OSD renderer before a texture is written
    again.

***************************************************************************/

#include "emu.h"
#include "vk3d_share.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include "bgfx/3rdparty/khronos/vulkan-local/vulkan_win32.h"
#endif


namespace {

constexpr VkFormat FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
constexpr size_t MIN_TARGETS = 3;
constexpr size_t MAX_TARGETS = 5;

} // anonymous namespace


struct vk3d_share::target
{
	int id = 0;                         // texture of the sink
	VkImage image = VK_NULL_HANDLE;     // imported in the device
	VkDeviceMemory memory = VK_NULL_HANDLE;
	bitmap_rgb32 bitmap;                // frame coordinates, filled by read()
	u64 given = 0;                      // frame it was last given to
	bool written = false;               // holds a frame (and the cleared margin)
	bool released = false;              // its frame was handed to the OSD renderer
	int tag = -1;
};


vk3d_share::vk3d_share(vk3d_context &context)
	: m_context(context)
	, m_alive(std::make_shared<bool>(true))
{
	VkCommandBufferAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	alloc_info.commandPool = m_context.command_pool;
	alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc_info.commandBufferCount = 1;
	if (m_context.command_pool)
		m_context.vkAllocateCommandBuffers(m_context.device, &alloc_info, &m_cmd);
	VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if (m_context.device)
		m_context.vkCreateFence(m_context.device, &fence_info, nullptr, &m_fence);
}


vk3d_share::~vk3d_share()
{
	*m_alive = false;
	if (!m_context.device)
		return;
	if (m_frames)
		osd_printf_verbose("vk3d_share: %u frames drawn by the display from %u shared textures, %u read back while they were all drawn\n", m_frames, u32(m_targets.size()), m_read_back);
	release_all();
	m_context.destroy_buffer(m_readback);
	if (m_fence)
		m_context.vkDestroyFence(m_context.device, m_fence, nullptr);
	// the command buffer goes with the pool
}


//-------------------------------------------------
//  supported - the device can write the textures
//  of the sink
//-------------------------------------------------

bool vk3d_share::supported(hires3d_gpu_sink &sink)
{
#if defined(_WIN32)
	if (!m_context.external_memory() || !m_cmd || !m_fence)
		return false;
	u8 luid[8];
	sink.adapter_luid(luid);
	if (std::memcmp(luid, m_context.luid(), sizeof(luid)))
	{
		osd_printf_verbose("vk3d_share: the display draws on another adapter, frames are read back\n");
		return false;
	}

	// a BGRA8 image imported from a Direct3D 11 texture
	VkPhysicalDeviceExternalImageFormatInfo external{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
	external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	VkPhysicalDeviceImageFormatInfo2 info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
	info.pNext = &external;
	info.format = FORMAT;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	VkExternalImageFormatProperties external_properties{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
	VkImageFormatProperties2 properties{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
	properties.pNext = &external_properties;
	if ((m_context.vkGetPhysicalDeviceImageFormatProperties2(m_context.physical, &info, &properties) != VK_SUCCESS) ||
			!(external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
	{
		osd_printf_verbose("vk3d_share: %s cannot import the display's textures, frames are read back\n", m_context.name);
		return false;
	}
	osd_printf_verbose("vk3d_share: frames handed to the display on %s without read-back\n", m_context.name);
	return true;
#else
	return false;
#endif
}


//-------------------------------------------------
//  create_target - a new texture of the shown
//  size, imported
//-------------------------------------------------

bool vk3d_share::create_target(hires3d_gpu_sink &sink)
{
#if defined(_WIN32)
	auto t = std::make_unique<target>();

	// the texture starts at the top left corner of the shown area and is as
	// wide as a row of the bitmap, as the OSD renderer sees the bitmap
	t->bitmap.allocate(m_shown.right() + 1, m_shown.bottom() + 1);
	int const width = t->bitmap.rowpixels();
	int const height = m_shown.height();
	void *handle = nullptr;
	t->id = sink.create(width, height, handle);
	if (!t->id)
		return false;

	VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.pNext = &external;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = FORMAT;
	info.extent = { u32(width), u32(height), 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bool ok = (m_context.vkCreateImage(m_context.device, &info, nullptr, &t->image) == VK_SUCCESS);
	if (ok)
	{
		VkMemoryRequirements requirements;
		m_context.vkGetImageMemoryRequirements(m_context.device, t->image, &requirements);
		u32 types = requirements.memoryTypeBits;
		VkMemoryWin32HandlePropertiesKHR handle_properties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
		auto const get_properties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(m_context.vkGetMemoryWin32HandlePropertiesKHR);
		if ((get_properties(m_context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, HANDLE(handle), &handle_properties) == VK_SUCCESS) &&
				(types & handle_properties.memoryTypeBits))
			types &= handle_properties.memoryTypeBits;
		int const type = m_context.find_memory(types, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.image = t->image;
		VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
		import.pNext = &dedicated;
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
		import.handle = HANDLE(handle);
		VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		alloc.pNext = &import;
		alloc.allocationSize = requirements.size;
		alloc.memoryTypeIndex = (type >= 0) ? u32(type) : 0;
		ok = (type >= 0) &&
				(m_context.vkAllocateMemory(m_context.device, &alloc, nullptr, &t->memory) == VK_SUCCESS) &&
				(m_context.vkBindImageMemory(m_context.device, t->image, t->memory, 0) == VK_SUCCESS);
	}
	if (!ok)
	{
		osd_printf_warning("vk3d_share: importing a texture of the display failed, frames are read back\n");
		destroy_target(*t);
		m_supported = 0;
		return false;
	}
	m_targets.push_back(std::move(t));
	return true;
#else
	return false;
#endif
}


void vk3d_share::destroy_target(target &t)
{
	if (t.image)
		m_context.vkDestroyImage(m_context.device, t.image, nullptr);
	if (t.memory)
		m_context.vkFreeMemory(m_context.device, t.memory, nullptr);
	t.image = VK_NULL_HANDLE;
	t.memory = VK_NULL_HANDLE;
	if (t.id && m_sink && (m_sink == hires3d_output::gpu_sink()))
		m_sink->destroy(t.id);
	t.id = 0;
}


void vk3d_share::release_all()
{
	if (m_targets.empty())
		return;
	m_context.vkDeviceWaitIdle(m_context.device);
	for (auto &t : m_targets)
		destroy_target(*t);
	m_targets.clear();
}


//-------------------------------------------------
//  acquire - the texture for the next frame
//-------------------------------------------------

vk3d_share::target *vk3d_share::acquire(rectangle const &shown)
{
	// the textures belong to the sink they came from (the old one is gone)
	hires3d_gpu_sink *const sink = hires3d_output::gpu_sink();
	if (sink != m_sink)
	{
		if (!sink)
			m_sink = nullptr;
		release_all();
		m_sink = sink;
		m_supported = -1;
	}
	if (!sink)
		return nullptr;
	if (m_supported < 0)
		m_supported = supported(*sink) ? 1 : 0;
	if (!m_supported || (shown.width() <= 0) || (shown.height() <= 0) || (shown.left() < 0) || (shown.top() < 0))
		return nullptr;
	if (shown != m_shown)
	{
		release_all();
		m_shown = shown;
	}

	// the oldest texture the OSD renderer is done drawing, not one of the
	// two given last nor one a screen holds (they may still be shown); the
	// textures are drawable a frame or two after they are made; waiting for
	// the OSD renderer here can stall it (this thread presents its frames),
	// so another texture is made instead and the frame is read back
	while ((m_targets.size() < MIN_TARGETS) && m_supported)
		if (!create_target(*sink))
			break;
	std::vector<target *> candidates;
	for (auto &t : m_targets)
		if ((!t->given || (t->given + 2 <= m_given)) && sink->ready(t->id) && !hires3d_output::gpu_texture_in_use(t->id))
			candidates.push_back(t.get());
	std::sort(candidates.begin(), candidates.end(), [] (target const *a, target const *b) { return a->given < b->given; });
	target *best = nullptr;
	for (target *t : candidates)
		if (sink->idle(t->id, false))
		{
			best = t;
			break;
		}
	if (!best)
	{
		if (!candidates.empty() && (m_targets.size() < MAX_TARGETS))
			create_target(*sink);
		if (!candidates.empty())
			m_read_back++;
		return nullptr;
	}
	best->released = false;
	best->given = ++m_given;
	m_frames++;
	return best;
}


//-------------------------------------------------
//  record - copy the frame into the texture
//-------------------------------------------------

void vk3d_share::record(VkCommandBuffer cmd, target &t, VkImage src, VkImageLayout src_layout, s32 x, s32 y, s32 first_row, s32 rows)
{
	VkImageSubresourceRange const range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	first_row = std::clamp<s32>(first_row, 0, m_shown.height());
	rows = std::clamp<s32>((rows < 0) ? m_shown.height() : rows, 0, m_shown.height() - first_row);
	if (!rows)
		return;

	// from the OSD renderer to this queue (the contents are kept once a
	// frame was written: the margin right of the frame stays cleared)
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = t.written ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = t.written ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = t.written ? m_context.queue_family : VK_QUEUE_FAMILY_IGNORED;
	barrier.image = t.image;
	barrier.subresourceRange = range;
	m_context.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	if (!t.written)
	{
		VkClearColorValue const black{ { 0.0f, 0.0f, 0.0f, 1.0f } };
		m_context.vkCmdClearColorImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		VkMemoryBarrier cleared{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		cleared.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		m_context.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &cleared, 0, nullptr, 0, nullptr);
		t.written = true;
	}

	VkImageCopy region{};
	region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.srcOffset = { x, y + first_row, 0 };
	region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.dstOffset = { 0, first_row, 0 };
	region.extent = { u32(m_shown.width()), u32(rows), 1 };
	m_context.vkCmdCopyImage(cmd, src, src_layout, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// to the OSD renderer
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = m_context.queue_family;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	m_context.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}


void vk3d_share::submitted(target &t, bool last)
{
	if (!last)
		return;
	t.released = true;
	if (m_sink && (m_sink == hires3d_output::gpu_sink()))
		m_sink->written(t.id);
}


bitmap_rgb32 &vk3d_share::bitmap(target &t)
{
	return t.bitmap;
}


hires3d_output::gpu_texture vk3d_share::texture(target &t)
{
	hires3d_output::gpu_texture result;
	result.id = t.id;
	std::weak_ptr<bool> const alive = m_alive;
	int const id = t.id;
	result.read = [this, alive, id] ()
	{
		// the target may be gone with the renderer or a new size
		if (alive.expired())
			return;
		for (auto &t : m_targets)
			if (t->id == id)
				read(*t);
	};
	return result;
}


vk3d_share::target *vk3d_share::find(bitmap_rgb32 const *frame)
{
	for (auto &t : m_targets)
		if (&t->bitmap == frame)
			return t.get();
	return nullptr;
}


int &vk3d_share::tag(target &t)
{
	return t.tag;
}


//-------------------------------------------------
//  read - copy the texture into the bitmap, for
//  the CPU; waits for the GPU
//-------------------------------------------------

void vk3d_share::read(target &t)
{
	int const width = m_shown.width();
	int const height = m_shown.height();
	VkDeviceSize const bytes = VkDeviceSize(width) * height * 4;
	if (!t.written || !t.released)
		return;
	if (m_readback.size < bytes)
	{
		m_context.destroy_buffer(m_readback);
		if (!m_context.create_buffer(m_readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true))
			return;
	}

	VkCommandBuffer const cmd = m_cmd;
	m_context.vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	m_context.vkBeginCommandBuffer(cmd, &begin);

	// after the batch that copied the frame, earlier on the queue
	VkImageSubresourceRange const range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	barrier.dstQueueFamilyIndex = m_context.queue_family;
	barrier.image = t.image;
	barrier.subresourceRange = range;
	m_context.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkBufferImageCopy copy{};
	copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copy.imageExtent = { u32(width), u32(height), 1 };
	m_context.vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_readback.handle, 1, &copy);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = 0;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = m_context.queue_family;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	m_context.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkBufferMemoryBarrier host{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = m_readback.handle;
	host.size = VK_WHOLE_SIZE;
	m_context.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	m_context.vkEndCommandBuffer(cmd);

	VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	m_context.vkResetFences(m_context.device, 1, &m_fence);
	if ((m_context.vkQueueSubmit(m_context.queue, 1, &submit, m_fence) != VK_SUCCESS) ||
			(m_context.vkWaitForFences(m_context.device, 1, &m_fence, VK_TRUE, ~u64(0)) != VK_SUCCESS))
		return;
	if (!m_readback.coherent)
	{
		VkMappedMemoryRange invalidate{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		invalidate.memory = m_readback.memory;
		invalidate.size = VK_WHOLE_SIZE;
		m_context.vkInvalidateMappedMemoryRanges(m_context.device, 1, &invalidate);
	}
	u32 const *const src = reinterpret_cast<u32 const *>(m_readback.mapped);
	for (int y = 0; y < height; y++)
		std::copy_n(&src[size_t(y) * width], width, &t.bitmap.pix(m_shown.top() + y, m_shown.left()));
}
