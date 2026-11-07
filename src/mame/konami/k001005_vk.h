// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    k001005_vk.h

    Vulkan renderer for the Konami K001005 at a higher resolution (mame3d).

    With -hires3d, the K001005 hands its triangles to this renderer instead
    of rasterizing them.  The GPU draws them into one image per frame
    buffer page and a shared depth image, with a port of the scanline
    renderer (K001006 texture ROM and palette, lighting, fog, frame buffer
    blends).  Batches are sent without waiting, the finished page is read
    back as soon as the pages are swapped, and the display copies its
    opaque pixels like the software renderer.  At the high resolution the
    drivers can instead have the GPU compose the screen (native 2D layers
    and 3D page) and show the read-back memory directly.

***************************************************************************/

#ifndef MAME_KONAMI_K001005_VK_H
#define MAME_KONAMI_K001005_VK_H

#pragma once

#include "hires3d.h"

#include <memory>
#include <string>


class k001005_vk
{
public:
	// a vertex in target pixels, with what the scanline renderer interpolates
	struct vertex
	{
		float x, y;
		float z, fog, diff;
		float u, v, w;              // u/w, v/w, 1/w
		float r, g, b, a;
	};

	// what the scanline renderer reads from its object data
	struct poly
	{
		u32 texture_x, texture_y;   // in texels
		u32 texture_width, texture_height; // size / 8 - 1
		u32 texture_page, texture_palette;
		bool mirror, fog, bilinear, texture, vertexcolor, fbblend, zcompare;
		rgb_t diffuse, ambient, fogcolor;
		u32 polycolor;              // ARGB
	};

	static std::unique_ptr<k001005_vk> create(int width, int height, std::string &error);
	~k001005_vk();

	std::string const &device_name() const;

	// the K001006 texture ROM (uploaded once) and palette (0x800 entries,
	// read with each batch)
	void set_texture_memory(u8 const *data, size_t bytes);
	void set_palette(pen_t const *pens);

	// drawing, in order, into a frame buffer page (0 or 1)
	void triangle(int page, poly const &p, vertex const *v, rectangle const &clip);
	void clear(int page);

	// start reading a finished page back, without waiting
	void prefetch(int page);

	// copy the opaque pixels of a page over the clip area of dest, taking
	// every step-th pixel (the software renderer's draw())
	bool draw(int page, bitmap_rgb32 &dest, rectangle const &clip, int step);

	// the screen at the high resolution: the native back layer, the opaque
	// pixels of the page (if with_3d), the opaque pixels of the native front
	// layer; clip is the native area the page covers, and the result is a
	// bitmap over the read-back memory in high-resolution screen pixels,
	// intact until the next frame is composed (nullptr if clip does not
	// match the page size)
	bitmap_rgb32 *compose(int page, bool with_3d, bitmap_rgb32 const &back, bitmap_rgb32 const &front, rectangle const &clip, int scale);

	// the texture of the display holding a frame compose() returned, if it
	// went there instead of being read back
	hires3d_output::gpu_texture shared_texture(bitmap_rgb32 const *frame);

private:
	struct impl;

	k001005_vk(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

#endif // MAME_KONAMI_K001005_VK_H
