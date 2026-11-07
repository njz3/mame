// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    namcos21_vk.h

    Vulkan renderer for the Namco System 21 3D at a higher resolution
    (mame3d).

    With -hires3d, the two frame buffers (pens and depth) live on the GPU
    at the high resolution.  The GPU rasterises the quads and tests their
    depth, and the screen is mixed there (Winning Run's 2D layer, or
    Driver's Eyes' native sprites) and converted through the palette, then
    read back.  Nothing is rasterised on the CPU.

***************************************************************************/

#ifndef MAME_NAMCO_NAMCOS21_VK_H
#define MAME_NAMCO_NAMCOS21_VK_H

#pragma once

#include "hires3d.h"

#include <memory>
#include <string>


class namcos21_vk
{
public:
	// what bitmap_draw uses for a band of Winning Run's screen
	struct layer_mix
	{
		int yscroll;
		int xscroll;
		u32 base;
		u32 pri;
		u32 fill;
	};

	// width and height: the native frame buffer
	static std::unique_ptr<namcos21_vk> create(int scale, int width, int height, std::string &error);
	~namcos21_vk();

	std::string const &device_name() const;

	// a quad of blit_single_quad: vertices in high-resolution frame buffer
	// pixels, pen and depth
	void quad(float const *x, float const *y, u16 color, int zsort);

	// swap_and_clear_poly_framebuffer
	void swap();

	// Winning Run: a write into the 2D layer (byte offset), and a screen
	// update mixing it with the 3D; once the bottom of the visible area is
	// reached, frame is a bitmap in high-resolution screen coordinates over
	// the read-back memory, intact until the next frame is shown, else
	// nullptr; false if the GPU failed
	void layer_written(offs_t offset);
	bool mix_layer(u8 const *layer, layer_mix const &mix, rgb_t const *palette, u32 colors, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame);

	// Driver's Eyes: a screen update with the native screen under the 3D
	// (bottom) and the pixels of the sprites over it (top, 0xffff where
	// they leave the pixel)
	bool mix_sprites(bitmap_ind16 const &bottom, bitmap_ind16 const &top, rgb_t const *palette, u32 colors, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame);

	// the frame returned last is read back: call before reading it (the
	// read-back is not waited for when the frame is returned, so that the
	// GPU works while the other screens are updated)
	void frame_ready();

	// the texture of the display holding a frame returned, if it went there
	// instead of being read back
	hires3d_output::gpu_texture shared_texture(bitmap_rgb32 const *frame);

private:
	struct impl;

	namcos21_vk(std::unique_ptr<impl> &&p);

	std::unique_ptr<impl> m;
};

#endif // MAME_NAMCO_NAMCOS21_VK_H
