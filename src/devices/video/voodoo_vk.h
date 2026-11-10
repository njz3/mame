// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    voodoo_vk.h

    Vulkan renderer for the Voodoo 1/2, Banshee and Voodoo 3 at a higher
    resolution (mame3d).

    The device keeps emulating the chip and its native frame buffer; with
    -hires3d it also hands every triangle, fast fill and CPU write to this
    renderer, which draws them on the GPU into one image per colour buffer
    (and one depth image) at N times the resolution.  The Voodoo pixel
    pipeline runs in a fragment shader that reads the texture memory, so
    no texture cache is needed; depth test, blending and order are done by
    the fixed-function stages.  The display reads the front buffer back,
    or draws it from a texture it shares (vk3d_share).

***************************************************************************/

#ifndef MAME_VIDEO_VOODOO_VK_H
#define MAME_VIDEO_VOODOO_VK_H

#pragma once

// included by voodoo.h, after voodoo_regs.h and voodoo_render.h

#include "hires3d.h"

#include <memory>
#include <string>


namespace voodoo {

class voodoo_vk
{
public:
	// what a triangle needs besides its poly_data
	struct triangle_state
	{
		u32 chroma_range;
		u32 tmu_config;
		u8 const *fogblend;     // 64 entries
		u8 const *fogdelta;     // 64 entries
		u8 fogdelta_mask;
		u8 bilinear_mask;
	};

	static std::unique_ptr<voodoo_vk> create(int scale, std::string &error);
	~voodoo_vk();

	std::string const &device_name() const;

	// the GPU stopped working
	bool failed() const;

	// the static texel tables, recognised by address: rgb332, alpha8, int8,
	// ai44 (256 entries), rgb565, argb1555, argb4444 (65536 entries)
	void set_static_lookups(rgb_t const *const tables[7]);

	// texture memory of each TMU; tmu_written() must be called before the
	// emulated memory changes
	void set_tmu_memory(int tmu, u8 const *ram, u32 size);
	void tmu_written(int tmu, u8 const *ptr, u32 bytes);

	// the texture memory changed as a whole (a state was loaded)
	void tmu_reload();

	// native layout of the colour and depth buffers (on Banshee and later,
	// buffer numbers are the device's copies of buffer addresses)
	void set_layout(u32 rowpixels, u32 rows);

	// drawing, in order; buffer is the colour buffer (0-2), positions and
	// poly_data are already magnified, x/scry are native frame buffer pixels
	void triangle(int buffer, bool has_aux, poly_data const &poly, voodoo_renderer::vertex_t const *vert, triangle_state const &state);
	void fastfill(int buffer, bool has_aux, poly_data const &poly);
	void write_color(int buffer, s32 x, s32 scry, u16 value);
	void write_depth(s32 x, s32 scry, u16 value);
	void fill_rows(int buffer, u32 first, u32 count, u16 value);

	// the CLUT of the DAC (256 red, green and blue values), applied on the
	// GPU before reading back; nullptr when it is linear
	void set_clut(u8 const *lut);

	// start copying a buffer back for the display, or into a texture of the
	// display, without waiting (after a swap, the buffer is shown from the
	// next frame); area, xoffs and yoffs are those of frame()
	void prefetch(int buffer, rectangle const &area, s32 xoffs, s32 yoffs);

	// draw what is pending and copy the hclip area of a buffer into dest:
	// dest pixel (x, y) is target pixel (x - xoffs, y - yoffs); lut holds
	// 256 red, green and blue values, or is nullptr
	bool read(int buffer, bitmap_rgb32 &dest, rectangle const &hclip, s32 xoffs, s32 yoffs, u8 const *lut);

	// same without copying: a bitmap over the read-back memory, in the
	// coordinates of read(), covering area; it stays intact until two other
	// images have been read back; nullptr if area is not inside the image;
	// or the bitmap of a texture of the display holding the area, whose
	// batch frame_ready() waits for, and shared_texture() gives the texture
	bitmap_rgb32 *frame(int buffer, rectangle const &area, s32 xoffs, s32 yoffs);
	void frame_ready(bitmap_rgb32 const *frame);
	hires3d_output::gpu_texture shared_texture(bitmap_rgb32 const *frame);

private:
	struct impl;

	voodoo_vk(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

} // namespace voodoo

#endif // MAME_VIDEO_VOODOO_VK_H
