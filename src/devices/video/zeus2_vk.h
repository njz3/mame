// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    zeus2_vk.h

    Vulkan renderer for the Midway Zeus 2 at a higher resolution (mame3d).

    With -hires3d, the high-resolution copy of the frame buffer (colour
    and depth, with the native row layout) lives on the GPU.  The polygons
    are drawn there by a port of render_triangle and render_poly_8bit, the
    fills and host writes are copied there as blocks, and the rows shown
    are read back.  Wave RAM (textures) is mirrored on the GPU, and the
    native frame buffer is still drawn by the software renderer for the
    emulation.

***************************************************************************/

#ifndef MAME_VIDEO_ZEUS2_VK_H
#define MAME_VIDEO_ZEUS2_VK_H

#pragma once

#include <memory>
#include <string>


class zeus2_vk
{
public:
	// a polygon vertex in native pixels: Z (12.12), u/z, v/z, 1/z
	struct vertex
	{
		float x, y;
		float p[4];
	};

	enum
	{
		MODE_SOLID,
		MODE_RGB555,
		MODE_ALPHA,         // 8-bit texels with 8-bit alpha
		MODE_TEXTURE        // palette texels
	};

	enum
	{
		TEXEL_4BIT_2X2,
		TEXEL_8BIT_4X2,
		TEXEL_8BIT_2X2,
		TEXEL_8BIT_2X2_ALPHA
	};

	// what render_poly_8bit uses from zeus2_poly_extra_data
	struct state
	{
		int mode;
		int texel_format;
		u32 texbase;        // wave RAM block (8 bytes)
		u32 texwidth;
		u32 solidcolor;     // RGB32
		u32 transcolor;
		u32 src_alpha;
		u32 dst_alpha;
		s32 zbuf_min;
		bool blend;
		bool depth_test;
		bool depth_write;
		bool depth_clear;
	};

	// frame_color and frame_depth: the native frame buffer (1M pixels);
	// waveram: wave RAM 0 (16 MB); palette: the 256-entry palette table
	static std::unique_ptr<zeus2_vk> create(int scale, u32 const *frame_color, s32 const *frame_depth, u32 const *waveram, u32 const *palette, std::string &error);
	~zeus2_vk();

	std::string const &device_name() const;

	// the frame buffer has rows of 1 << row_shift pixels; -widescreen: each
	// row has pad more high-resolution columns on each side, where the
	// polygons whose clip reaches the edges of the screen (visible_width
	// native columns) go on, and that the fills of whole rows clear
	void set_layout(int row_shift, int pad = 0, int visible_width = 0);

	// a polygon of zeus2_render_poly as render_triangle_fan draws it: native
	// vertices, window origin (frame buffer address) and clip
	void polygon(state const &st, u32 window, rectangle const &clip, int numverts, vertex const *v);

	// a fast clear of count pixels from addr, and a host write of a pixel
	void fill(u32 addr, u32 count, u32 color, s32 depth);
	void pixel(u32 addr, u32 color, bool write_depth, s32 depth);

	// a host write into wave RAM (byte offset), before it is done
	void waveram_written(offs_t offset);

	// the palette table was loaded
	void palette_changed();

	// the frame buffer, wave RAM and palette were replaced (state load)
	void resync();

	// the display starts at a new frame buffer address: start reading it back
	void prefetch(u32 display, rectangle const &visible);

	// a screen update showing the cliprect rows from the display address;
	// once the bottom of the visible area is reached, frame is a bitmap in
	// high-resolution frame coordinates (the screen starts pad columns in)
	// over the read-back memory, intact
	// until the next frame is shown, else nullptr; false if the GPU failed
	bool update(u32 display, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame);

private:
	struct impl;

	zeus2_vk(std::unique_ptr<impl> &&p);

	std::unique_ptr<impl> m;
};

#endif // MAME_VIDEO_ZEUS2_VK_H
