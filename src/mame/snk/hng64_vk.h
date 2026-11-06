// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    hng64_vk.h

    Vulkan renderer for the SNK Hyper Neo Geo 64 3D at a higher resolution
    (mame3d).

    With -hires3d, the 3D colour and depth buffers live on the GPU at the
    high resolution.  The GPU rasterises the polygons, interpolates their
    parameters and tests their depth; a fragment shader computes the chip's
    pixels with the texture ROM uploaded once.  The screen is mixed there
    (native layers under the 3D, the 3D blit with its scroll, stretch,
    palette and blend, then the layers over it) and read back.  Nothing is
    rasterised on the CPU.

***************************************************************************/

#ifndef MAME_SNK_HNG64_VK_H
#define MAME_SNK_HNG64_VK_H

#pragma once

#include <memory>
#include <string>


class hng64_vk
{
public:
	// a vertex in native 3D buffer pixels: Z, W, light, S, T
	struct vertex
	{
		float x, y;
		float p[5];
	};

	// what the scanline renderers read from hng64_poly_data
	struct state
	{
		bool flat;
		bool tex4bpp;
		bool blend;
		u32 tex_index;
		u32 pal_offset;
		u32 color_index;
		u32 tex_page_small;
		u32 texscrollx, texscrolly;
		u32 tex_mask_x, tex_mask_y;
	};

	// what blit_3d reads
	struct blit
	{
		bool enabled;
		int xscroll;        // native
		int yinc;           // 16.16
		u32 palbase;
	};

	// size: the native 3D buffers (square); texture: 16 MB of texels
	static std::unique_ptr<hng64_vk> create(int scale, int size, u8 const *texture, std::string &error);
	~hng64_vk();

	std::string const &device_name() const;

	// a polygon of drawShaded, triangles (0, j, j + 1) as it draws them
	void polygon(state const &st, int numverts, vertex const *v);

	// clear3d
	void clear();

	// a screen update with the native screen under the 3D (bottom) and the
	// layers over it (top: alpha 0xff when opaque, else added to alpha / 128
	// times what is under them); once the
	// bottom of the visible area is reached, frame is a bitmap in
	// high-resolution screen coordinates over the read-back memory, intact
	// until the next frame is shown, else nullptr; false if the GPU failed
	bool mix(bitmap_rgb32 const &bottom, bitmap_rgb32 const &top, blit const &b, rgb_t const *palette, u32 colors, rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame);

	// the frame returned last is read back: call before reading it
	void frame_ready();

private:
	struct impl;

	hng64_vk(std::unique_ptr<impl> &&p);

	std::unique_ptr<impl> m;
};

#endif // MAME_SNK_HNG64_VK_H
