// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    gaelco3d_vk.h

    Vulkan renderer for the Gaelco 3D hardware at a higher resolution
    (mame3d).

    With -hires3d, the frame and depth buffers live on the GPU at the high
    resolution.  The GPU rasterises the polygons, and a fragment shader
    computes the chip's pixels (plane equations, texture mask, COLOR3D
    filter, 50% blend) with the texture ROM uploaded once.  The rows shown
    are read back.  Nothing is rasterised on the CPU.

***************************************************************************/

#ifndef MAME_GAELCO_GAELCO3D_VK_H
#define MAME_GAELCO_GAELCO3D_VK_H

#pragma once

#include <memory>
#include <string>


class gaelco3d_vk
{
public:
	struct vertex
	{
		float x, y;
	};

	enum
	{
		MODE_NOZ,           // render_noz_noperspective
		MODE_NORMAL,        // render_normal
		MODE_ALPHA          // render_alphablend
	};

	// gaelco3d_object_data, gradients at the high resolution
	struct plane
	{
		u32 tex, color;
		float z0;
		float ooz_base, ooz_dx, ooz_dy;
		float uoz_base, uoz_dx, uoz_dy;
		float voz_base, voz_dx, voz_dy;
	};

	// width and height: the screen; texture and texmask: the expanded ROMs
	// (one byte per texel, one flag byte per masked texel); palette: the
	// 32768 colours the scanline renderers read
	static std::unique_ptr<gaelco3d_vk> create(int scale, int width, int height, u8 const *texture, u32 texture_size, u8 const *texmask, u32 texmask_size, rgb_t const *palette, std::string &error);
	~gaelco3d_vk();

	std::string const &device_name() const;

	// a polygon of render_poly, a triangle fan, vertices at the high
	// resolution
	void polygon(int mode, plane const &p, int numverts, vertex const *v);

	// a palette write
	void palette_changed();

	// a screen update showing the cliprect rows; once the bottom of the
	// visible area is reached, frame is a bitmap in high-resolution screen
	// coordinates over the read-back memory, intact until the next frame is
	// shown, else nullptr; false if the GPU failed
	bool update(rectangle const &cliprect, rectangle const &visible, bitmap_rgb32 *&frame);

	// the frame returned last is read back: call before reading it
	void frame_ready();

private:
	struct impl;

	gaelco3d_vk(std::unique_ptr<impl> &&p);

	std::unique_ptr<impl> m;
};

#endif // MAME_GAELCO_GAELCO3D_VK_H
