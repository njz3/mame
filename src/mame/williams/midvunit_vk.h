// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    midvunit_vk.h

    Vulkan renderer for the Midway V-Unit at a higher resolution (mame3d).

    With -hires3d, the high-resolution copy of the two video RAM pages lives
    on the GPU.  The quads of the DMA queue are drawn there by a port of
    the scanline renderers, the CPU writes into video RAM are copied there
    as blocks, and the page shown is converted through the palette into a
    display image that is read back.  The native video RAM is still drawn
    by the software renderer for the emulation.

***************************************************************************/

#ifndef MAME_WILLIAMS_MIDVUNIT_VK_H
#define MAME_WILLIAMS_MIDVUNIT_VK_H

#pragma once

#include <memory>
#include <string>


class midvunit_vk
{
public:
	// a quad vertex in target pixels, with the texture parameters (16.16
	// texels) the scanline renderers interpolate
	struct vertex
	{
		float x, y;
		float u, v;
	};

	enum
	{
		MODE_FLAT,          // render_flat
		MODE_TEX,           // render_tex
		MODE_TEXTRANS,      // render_textrans
		MODE_TEXTRANSMASK   // render_textransmask
	};

	// vram: both native pages (2 x 512 x 512); texture: the texture RAM bytes
	static std::unique_ptr<midvunit_vk> create(int scale, u16 const *vram, u8 const *texture, std::string &error);
	~midvunit_vk();

	std::string const &device_name() const;

	// a quad of the DMA queue, as process_dma_queue renders it, with the
	// vertices and clip at the high resolution
	void quad(int page, vertex const *v, int mode, u16 pixdata, bool dither, u32 texbase, rectangle const &clip);

	// a CPU write into video RAM (native coordinates)
	void write(int page, int x, int y, u16 data);

	// a CPU write into texture RAM (byte offset)
	void texture_written(offs_t offset);

	// video RAM and texture RAM were replaced (state load)
	void resync();

	// a page becomes visible: start converting it for the display
	void prefetch(int page, rectangle const &visible, rgb_t const *pens);

	// a screen update showing the cliprect rows of a page through pens
	// (32768 colours); once the bottom of the visible area is reached, frame
	// is a bitmap in high-resolution screen coordinates over the read-back
	// memory, intact until the next frame is shown, else nullptr; false if
	// the GPU failed
	bool update(int page, rectangle const &cliprect, rectangle const &visible, rgb_t const *pens, bitmap_rgb32 *&frame);

private:
	struct impl;

	midvunit_vk(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

#endif // MAME_WILLIAMS_MIDVUNIT_VK_H
