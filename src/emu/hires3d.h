// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    hires3d.h

    High-resolution output for emulated 3D hardware (mame3d).

    A driver whose 3D renderer can draw at a multiple of the native
    resolution creates a hires3d_output for its screen.  When the
    -hires3d option is above 1 and the screen is displayed, the driver
    draws each frame into frame_bitmap() (the native size times the
    scale factor), magnifies its 2D layers into it with upscale(), and
    calls frame_done().  The screen then gives that bitmap to the
    renderer instead of its native bitmap.  Emulation timing, partial
    updates and the native bitmap are not affected.

***************************************************************************/

#ifndef MAME_EMU_HIRES3D_H
#define MAME_EMU_HIRES3D_H

#pragma once

#include <memory>

#define OPTION_HIRES3D "hires3d"

class device_palette_interface;
class screen_device;
class vk3d_renderer;


class hires3d_output
{
public:
	hires3d_output(screen_device &screen);
	~hires3d_output();

	// scale factor requested with -hires3d (1 = native resolution)
	int scale() const { return m_scale; }

	// true when the driver should draw a high-resolution frame
	bool active() const;

	// bitmap for the frame being drawn (native size times scale)
	bitmap_rgb32 &frame_bitmap();

	// the update covering cliprect is finished; the frame is shown once
	// the bottom of the visible area has been drawn
	void frame_done(const rectangle &cliprect);

	// native rectangle to high-resolution rectangle
	rectangle scaled(const rectangle &native) const;

	// magnify the cliprect area of a native bitmap into dest; when
	// transparent is set, pixels with a zero alpha are skipped
	void upscale(const bitmap_rgb32 &source, bitmap_rgb32 &dest, const rectangle &cliprect, bool transparent) const;

	// same for indexed bitmaps; pixels equal to transparent_pen are skipped
	// when it is not negative
	void upscale(const bitmap_ind16 &source, bitmap_ind16 &dest, const rectangle &cliprect, int transparent_pen = -1) const;

	// magnify into the frame bitmap the pixels of after that differ from
	// before (both native), such as the 2D layers drawn over the 3D
	void magnify_changes(const bitmap_rgb32 &before, const bitmap_rgb32 &after, const rectangle &cliprect);

	// convert the hclip area of a high-resolution indexed bitmap into the
	// frame bitmap through the palette, as the renderer does for an
	// indexed screen
	void indexed_to_frame(const bitmap_ind16 &source, const rectangle &hclip, device_palette_interface &palette);

	// GPU renderer, created on first use; nullptr when -hires3d_renderer is
	// software or Vulkan is not available (the driver then draws in software)
	vk3d_renderer *vulkan();
	void disable_vulkan(const char *reason);

	// called by screen_device: points texture at the last finished frame
	// and returns true, or returns false to keep the native bitmap
	static bool update_texture(screen_device &screen, render_texture &texture);

private:
	screen_device &m_screen;
	int m_scale;
	std::unique_ptr<vk3d_renderer> m_vulkan;
	bool m_vulkan_tried = false;
	bitmap_rgb32 m_bitmap[2];
	int m_current = 0;
	int m_published = -1;
	int m_shown = -1;
	u64 m_frame = ~u64(0);
};

#endif // MAME_EMU_HIRES3D_H
