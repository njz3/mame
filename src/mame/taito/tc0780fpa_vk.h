// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    tc0780fpa_vk.h

    Vulkan renderer for the Taito TC0780FPA at a higher resolution (mame3d).

    With -hires3d, the two frame buffers (pens and depth) live on the GPU
    at the high resolution.  The GPU rasterises the triangles and quads
    themselves, and a fragment shader computes the chip's solid, shaded and
    textured pixels, with texture RAM mirrored on the GPU.  The Taito JC
    screen is mixed there (native layers under and over the 3D) and
    converted through the palette, then read back.  Nothing is rasterised
    on the CPU.

***************************************************************************/

#ifndef MAME_TAITO_TC0780FPA_VK_H
#define MAME_TAITO_TC0780FPA_VK_H

#pragma once

#include <memory>
#include <string>


class tc0780fpa_vk
{
public:
	// a vertex in native pixels: Z, then colour or U, V and palette
	struct vertex
	{
		float x, y;
		float p[4];
	};

	enum
	{
		MODE_SOLID,         // render_solid_scan
		MODE_SHADE,         // render_shade_scan
		MODE_TEXTURE        // render_texture_scan
	};

	// what render_texture_scan reads from tc0780fpa_polydata
	struct texture
	{
		int base_x, base_y;
		int wrap_x, wrap_y;
	};

	// width and height: the screen; texture_ram: 2048 x 2048 texels
	static std::unique_ptr<tc0780fpa_vk> create(int scale, int width, int height, u8 const *texture_ram, std::string &error);
	~tc0780fpa_vk();

	std::string const &device_name() const;

	// a triangle or a quad of the chip
	void triangle(vertex const *v, int mode, texture const &tex);
	void quad(vertex const *v, int mode, texture const &tex);

	// swap_buffers
	void swap();

	// a write into texture RAM (byte offset), before it is done; state load
	void texture_written(offs_t offset);
	void resync();

	// a screen update with the native layers under the 3D (bottom) and over
	// it (top, 0xffff where they leave the pixel); once the bottom of the
	// visible area is reached, frame is a bitmap in high-resolution screen
	// coordinates over the read-back memory, intact until the next frame is
	// shown, else nullptr; false if the GPU failed
	bool mix(bitmap_ind16 const &bottom, bitmap_ind16 const &top, rgb_t const *palette, u32 colors, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame);

	// the frame returned last is read back: call before reading it
	void frame_ready();

private:
	struct impl;

	tc0780fpa_vk(std::unique_ptr<impl> &&p);

	std::unique_ptr<impl> m;
};

#endif // MAME_TAITO_TC0780FPA_VK_H
