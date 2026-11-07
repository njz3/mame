// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    namcos22_vk.h

    Vulkan renderer for Namco System 22 / Super System 22 at a higher
    resolution (mame3d).

    With -hires3d, the driver hands each frame's sorted polygons and sprites
    to this renderer instead of rasterizing them.  The GPU draws them in
    that order with a port of the scanline renderers (textures from the
    texture ROMs, palette, shading, fog, fades, alpha), then mixes the
    native text layer over them and applies the fades and gamma in a second
    pass.  The result is read back and shown without copying it, or copied
    into a texture of the display when it can draw one (vk3d_share).

***************************************************************************/

#ifndef MAME_NAMCO_NAMCOS22_VK_H
#define MAME_NAMCO_NAMCOS22_VK_H

#pragma once

#include "hires3d.h"

#include <memory>
#include <string>


class namcos22_vk
{
public:
	// a polygon vertex in screen pixels at the high resolution; p holds
	// 1/z, u/z, v/z and i/z for polygons, the texel x and y for sprites
	struct vertex
	{
		float x, y;
		float p[4];
	};

	// what the scanline renderers read from their object data
	enum { KIND_POLY, KIND_POLY_SUPER, KIND_SPRITE };
	struct draw
	{
		int kind;
		u32 penbase, penmask, penshift;  // palette index of pen 0, pen selection
		u32 bank;                        // texture bank * 0x1000
		bool shade, texture, alpha_all, polyfade;
		int fogfactor;                   // blend factor against the fog (0xff: none)
		rgb_t fogcolor;
		int fadefactor;                  // same for the fade
		rgb_t fadecolor;
		rgb_t polycolor;                 // polygon fade scale
		int alpha;                       // blend factor against the destination (0xff: opaque)
		u32 alphapen;
		u32 prio;                        // prioverchar
		u32 source, modulo;              // sprite: first byte of the tile, bytes per line
		int flipx, flipy;
	};

	// what the final mix needs besides the scene
	struct mix_params
	{
		bool super;
		bool text_before;                // Super: mix the text over the background first
		rgb_t background;
		bool fade_on;
		int fade_r, fade_g, fade_b;      // System 22: scales (0x100 = 1), Super: colour
		int fade_factor;                 // Super: blend factor against the fade colour
		bool shadow_on;                  // System 22
		rgb_t mix[3];
		u8 alpha_check12, alpha_check13, alpha_mask, alpha_factor; // Super text alpha
		bool spot_on;                    // Super spot
		int spot_factor, spot_palbase, text_palbase;
		u16 const *spotram;              // 0x800 entries, or nullptr
		u8 gamma[3][256];
	};

	static std::unique_ptr<namcos22_vk> create(std::string &error);
	~namcos22_vk();

	std::string const &device_name() const;

	// ROM data, uploaded once
	void set_texture_memory(u16 const *tilemap, u8 const *tileattr, u8 const *ayx, u8 const *tiledata, size_t tiledata_bytes);
	void set_sprite_memory(u8 const *data, size_t bytes);

	// a frame of the visible area at scale times its size; polygons are
	// convex fans of 3 to 6 vertices drawn in order, clipped to clip
	// (screen pixels at the high resolution)
	void begin_frame(rectangle const &visible, int scale, rgb_t background);
	void polygon(draw const &d, vertex const *v, int count, rectangle const &clip);

	// mix the native text layer and its priority map (screen coordinates),
	// start reading the frame back and return a bitmap over it in screen
	// coordinates at the high resolution, intact until two more frames are
	// drawn; its pixels are there once frame_ready() returns for it
	bitmap_rgb32 *end_frame(pen_t const *palette, size_t entries, bitmap_ind16 const &text, bitmap_ind8 const &textpri, mix_params const &mix);
	void frame_ready(bitmap_rgb32 const *frame);

	// the texture of the display holding a frame end_frame() returned, if
	// it went there instead of being read back
	hires3d_output::gpu_texture shared_texture(bitmap_rgb32 const *frame);

private:
	struct impl;

	namcos22_vk(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

#endif // MAME_NAMCO_NAMCOS22_VK_H
