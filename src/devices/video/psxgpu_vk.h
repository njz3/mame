// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    psxgpu_vk.h

    Vulkan renderer for the PlayStation GPU at a higher resolution (mame3d):
    Namco System 10/11/12, Konami GQ/GV/573, Sony ZN, Taito GNET.

    The software renderer keeps drawing into the native VRAM, which the
    games read and which the textures come from.  The same primitives are
    drawn on the GPU into a copy of the VRAM at the high resolution, in the
    order they come, and the display area is taken from that copy.

***************************************************************************/

#ifndef MAME_VIDEO_PSXGPU_VK_H
#define MAME_VIDEO_PSXGPU_VK_H

#pragma once

#include "hires3d.h"

#include <memory>
#include <string>

class psxgpu_vk
{
public:
	struct vertex
	{
		float x, y;             // VRAM pixels
		float r, g, b;          // 0-255, 128 is neutral on a texture
		float u, v;
	};

	struct primitive
	{
		bool textured;
		bool raw;               // the texture is not modulated by the colour
		bool semi;              // semi-transparent
		bool interleaved;
		u8 abr;                 // semi-transparency mode
		u8 tp;                  // texture depth
		u16 tx, ty;
		u16 clut_x, clut_y;
		u8 tww, twh;            // texture window masks
		s32 clip_left, clip_top, clip_right, clip_bottom;       // inclusive
	};

	static std::unique_ptr<psxgpu_vk> create(int scale, std::string &error);
	~psxgpu_vk();

	std::string const &device_name() const;

	// triangles of a primitive, count * 3 vertices
	void draw(primitive const &p, vertex const *v, int count);

	// a rectangle filled, ignoring the drawing area
	void fill(int x, int y, int w, int h, rgb_t color);

	// the VRAM is copied at the start of each batch, so a batch must end
	// before a part of it changes that a texture of the batch may be
	void flush(u16 const *const *vram);

	// a VRAM rectangle written by the CPU (to do after a flush)
	void upload(int x, int y, int w, int h);

	// a VRAM rectangle moved
	void move(int sx, int sy, int dx, int dy, int w, int h);

	// the frame: the VRAM area x, y, w, h at left, top of a screen_w x
	// screen_h screen; handed to a texture of the display when it can be
	bool show(u16 const *const *vram, int x, int y, int w, int h, int left, int top, int screen_w, int screen_h, hires3d_output &output, rectangle const &shown);

	void frame_ready();

private:
	struct impl;

	psxgpu_vk(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

#endif // MAME_VIDEO_PSXGPU_VK_H
