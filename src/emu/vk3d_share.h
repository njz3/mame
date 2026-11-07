// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    vk3d_share.h

    Frames of a GPU renderer handed to the OSD renderer (mame3d).

    When the OSD renderer registers a hires3d_gpu_sink for the adapter
    the renderer's Vulkan device runs on, each finished frame is copied
    on the GPU into one of three textures of the sink, imported in the
    device, instead of being read back; the OSD renderer draws that
    texture.  The frame bitmap is only filled when the CPU needs it.

    Only files that talk to Vulkan include this header.

***************************************************************************/

#ifndef MAME_EMU_VK3D_SHARE_H
#define MAME_EMU_VK3D_SHARE_H

#pragma once

#include "hires3d.h"
#include "vk3d_context.h"

#include <memory>
#include <vector>


class vk3d_share
{
public:
	struct target;

	vk3d_share(vk3d_context &context);
	~vk3d_share();

	// the texture for the next frame, whose shown area is shown (frame
	// coordinates, pixel for pixel in the image the frame is copied from),
	// or nullptr when the frame must be read back instead (no sink, another
	// adapter, textures not drawable yet, all of them still drawn); the two
	// textures given last are never returned
	target *acquire(rectangle const &shown);

	// record in cmd the copy of the shown area of src (its top left corner
	// at x, y; src in layout src_layout and readable by transfers) into the
	// texture, or of rows of it from first_row; the frame is drawable once
	// the commands copying all of it are done
	void record(VkCommandBuffer cmd, target &t, VkImage src, VkImageLayout src_layout, s32 x, s32 y, s32 first_row = 0, s32 rows = -1);

	// to call once each batch with copies into the texture is submitted;
	// the last batch of the frame hands it to the OSD renderer
	void submitted(target &t, bool last);

	// what to give to hires3d_output::use_frame()
	bitmap_rgb32 &bitmap(target &t);
	hires3d_output::gpu_texture texture(target &t);

	// the target whose bitmap is frame, or nullptr
	target *find(bitmap_rgb32 const *frame);

	// a value the renderer keeps with a target (the batch that copies into
	// it, for instance)
	int &tag(target &t);

	// frames handed over so far
	u32 frames() const { return m_frames; }

private:
	bool supported(hires3d_gpu_sink &sink);
	bool create_target(hires3d_gpu_sink &sink);
	void destroy_target(target &t);
	void release_all();
	void read(target &t);

	vk3d_context &m_context;
	hires3d_gpu_sink *m_sink = nullptr;      // the sink the textures belong to
	int m_supported = -1;                    // sharing works with it (-1: not checked)
	rectangle m_shown;                       // shown area of the textures
	std::vector<std::unique_ptr<target>> m_targets;
	u64 m_given = 0;                         // frames given to textures
	u32 m_frames = 0;
	u32 m_read_back = 0;                     // frames read back: every texture was still drawn
	VkCommandBuffer m_cmd = VK_NULL_HANDLE;  // for read()
	VkFence m_fence = VK_NULL_HANDLE;
	vk3d_context::buffer m_readback;
	std::shared_ptr<bool> m_alive;           // read callbacks check it
};

#endif // MAME_EMU_VK3D_SHARE_H
