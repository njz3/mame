// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    namcos23_vk.h

    Vulkan renderer for the Namco System 23 / Super System 23 3D at a higher
    resolution (mame3d).

    The sorted polygons of a frame are sent to the GPU, which rasterises
    them in that order; a fragment shader computes the chip's pixels from
    the texture ROM uploaded once.  A second pass mixes the text layer over
    them and applies the gamma tables.

***************************************************************************/

#ifndef MAME_NAMCO_NAMCOS23_VK_H
#define MAME_NAMCO_NAMCOS23_VK_H

#pragma once

#include "hires3d.h"

#include <memory>
#include <string>

class namcos23_vk
{
public:
	// a vertex, relative to the viewport like the software renderer's
	struct vertex
	{
		float x, y;
		float z, u, v, i;       // z, u/z, v/z, intensity/z as interpolated
	};

	// what a polygon needs at its pixels
	struct polygon
	{
		bool stencil, shade, poly_fade, color_fade, blend, poly_alpha, alpha_enabled;
		u32 tbase;
		u8 cmode;
		u32 palette;            // first colour, index in the palette
		float intensity_base;
		s32 fade_factor, fade_factor_inv;
		s32 fade_r, fade_g, fade_b;
		s32 poly_r, poly_g, poly_b;
		s32 alpha;              // 0x00-0xff
		u8 alpha_pen;
		s32 clip_left, clip_top, clip_right, clip_bottom;       // native pixels
	};

	// the text layer over the polygons
	struct text_mix
	{
		bool fade;
		s32 fade_factor, fade_factor_inv;
		s32 fade_r, fade_g, fade_b;
		s32 alpha_factor, alpha_factor_inv;     // alpha_factor 0: no alpha
		u32 alpha_mask, alpha_low, alpha_high;
	};

	// scale: of the native 640x480; pad: more high-resolution columns on each
	// side (-widescreen); tiles: one word per tile id, its texel offset with
	// the attributes in the low byte
	static std::unique_ptr<namcos23_vk> create(int scale, int pad, bool stretch2d, u8 const *texels, size_t texel_bytes, u32 const *tiles, u32 tile_count, u32 tileid_mask, std::string &error);
	~namcos23_vk();

	std::string const &device_name() const;

	void begin_frame(rgb_t background);
	void add_polygon(polygon const &p, vertex const *v, int count);

	// the text pens (640x480, 0xffff where there is none), the palette, the
	// three gamma tables and the stencil RAM; the frame is drawn, then given
	// to the output (a texture of the display or a bitmap it reads back)
	bool end_frame(pen_t const *palette, u32 colors, u16 const *text, text_mix const &mix, u16 const *gamma, u16 const *stencil, hires3d_output &output, rectangle const &shown);

	void frame_ready();

private:
	struct impl;

	namcos23_vk(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

#endif // MAME_NAMCO_NAMCOS23_VK_H
