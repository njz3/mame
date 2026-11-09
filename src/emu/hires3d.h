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

    A GPU renderer can also leave the frame on the GPU: when the OSD
    renderer registers a hires3d_gpu_sink (bgfx on Direct3D 11), the GPU
    renderer copies the frame into a texture of the sink instead of
    reading it back, and the OSD renderer draws that texture.  The bitmap
    is then only filled when the CPU needs the frame (snapshots and
    recordings).

***************************************************************************/

#ifndef MAME_EMU_HIRES3D_H
#define MAME_EMU_HIRES3D_H

#pragma once

#include <functional>
#include <memory>

#define OPTION_HIRES3D "hires3d"

// mame3d display options, as in the other emulators of the cabinets: the
// shape of the frame the screens are shown in, centred in the window or
// screen with black bars around (see render.cpp)
#define OPTION_ASPECT_RATIO "aspect_ratio"
#define OPTION_CROP_POSITION "crop_position"

// mame3d widescreen hack, for the chips whose 3D is projected in MAME
#define OPTION_WIDESCREEN "widescreen"
#define OPTION_WIDESCREEN_2D "widescreen_2d"
#define OPTION_WIDESCREEN_BG "widescreen_bg"

class device_palette_interface;
class screen_device;
class vk3d_renderer;


// textures of the OSD renderer that GPU renderers of the emulation write,
// registered by the OSD renderer that can draw them (hires3d_output::
// set_gpu_sink); everything happens on the main thread
class hires3d_gpu_sink
{
public:
	virtual ~hires3d_gpu_sink() = default;

	// LUID of the adapter that draws (8 bytes): only a GPU renderer on the
	// same adapter can write the textures
	virtual void adapter_luid(u8 *luid) const = 0;

	// create a BGRA8 texture of width x height pixels that a GPU renderer
	// writes through handle (an NT handle of a Direct3D 11 texture, valid
	// until the texture is destroyed); returns its id (above 0), or 0
	virtual int create(int width, int height, void *&handle) = 0;

	// true once the texture can be drawn (not right after it is created)
	virtual bool ready(int id) const = 0;

	// written() tells that a frame was copied into the texture (the GPU
	// renderer waits for it before the frame is drawn); idle() is true once
	// the draws that read the texture are done on the GPU, and the GPU
	// renderer then writes it again (with wait, it waits for the draws)
	virtual void written(int id) = 0;
	virtual bool idle(int id, bool wait) = 0;

	virtual void destroy(int id) = 0;
};


class hires3d_output
{
public:
	hires3d_output(screen_device &screen);
	~hires3d_output();

	// scale factor requested with -hires3d (1 = native resolution)
	int scale() const { return m_scale; }

	// -widescreen: a chip whose 3D is projected in MAME can draw a 16:9 frame
	// (allow_widescreen(), once its renderer can); the frame then has pad()
	// more high-resolution pixels on each side of the native screen, which
	// is shown at 16:9: the 3D gets a wider field of view, the 2D stays in
	// the middle at its own shape or is stretched over the whole width
	// (stretch_2d()), and background tilemaps may be drawn over the whole
	// width (wide_bg()); it works at the native resolution too
	void allow_widescreen(bool allow) { m_wide_allowed = allow; }
	bool wide() const { return m_wide_option && m_wide_allowed && (m_wide_factor > 1.0f); }
	int pad() const;
	bool stretch_2d() const { return m_stretch_2d; }
	bool wide_bg() const { return m_wide_bg; }

	// for the renderer: the factor the shape of the screens is multiplied by
	// (the widescreen frames are wider than the screen)
	static float view_widening();

	// true when the driver should draw a high-resolution frame
	bool active() const;

	// bitmap for the frame being drawn (native size times scale)
	bitmap_rgb32 &frame_bitmap();

	// a frame also held by a texture of the GPU sink, covering the shown
	// area from its top left corner: the OSD renderer draws the texture,
	// and read() fills the bitmap with it when the CPU needs the frame
	struct gpu_texture
	{
		int id = 0;
		std::function<void ()> read;
	};

	// show bitmap for the frame being drawn instead of the frame bitmap
	// (frame_bitmap() then returns it); it is in the same coordinates and
	// must stay intact until the next frame is shown; ready, if given, is
	// called just before the renderer reads the bitmap, after the throttle
	// (a GPU read-back can complete meanwhile), or when the next frame
	// takes the texture if the renderer did not read it; the bitmap must
	// not be written by the CPU once it is given; with a texture, the
	// bitmap is only filled by texture.read()
	void use_frame(bitmap_rgb32 &bitmap, std::function<void ()> &&ready = nullptr, gpu_texture &&texture = gpu_texture());

	// the update covering cliprect is finished; the frame is shown once
	// the bottom of the visible area has been drawn
	void frame_done(const rectangle &cliprect);

	// native rectangle to high-resolution rectangle (moved by pad())
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

	// called by the renderer before it reads a texture: calls the ready
	// callbacks of the frames given to the screens, so a GPU read-back runs
	// while the emulation is throttled instead of stalling it
	static void complete_reads();

	// the memory of a frame given to use_frame goes away: the renderer may
	// still be drawing that frame, and goes on from a copy (call it before
	// freeing the memory)
	static void frames_freed(void const *start, size_t bytes);

	// the sink of the OSD renderer, or nullptr (the OSD renderer sets it
	// when it starts and clears it before it stops)
	static void set_gpu_sink(hires3d_gpu_sink *sink);
	static hires3d_gpu_sink *gpu_sink();

	// for the OSD renderer: the texture of the sink holding the frame of a
	// screen texture whose first pixel is base, or 0
	static int gpu_texture_of(void const *base);

	// true if a frame given to a screen is in texture id (it must not be
	// written again)
	static bool gpu_texture_in_use(int id);

	// the frames in textures of the sink are copied into their bitmaps:
	// all of them before the CPU reads the screens (snapshots, recordings),
	// or the one at base when the OSD renderer cannot draw its texture
	static void read_gpu_textures();
	static void read_gpu_texture(void const *base);

private:
	void begin_frame();

	screen_device &m_screen;
	int m_scale;
	bool m_wide_option = false;             // -widescreen
	bool m_wide_allowed = false;            // the chip can draw it
	bool m_stretch_2d = false;              // -widescreen_2d stretch
	bool m_wide_bg = false;                 // -widescreen_bg
	float m_wide_factor = 1.0f;             // 16:9 over the shape of the screen
	std::unique_ptr<vk3d_renderer> m_vulkan;
	bool m_vulkan_tried = false;
	bitmap_rgb32 m_bitmap[2];
	int m_current = 0;
	bitmap_rgb32 *m_draw = nullptr;         // bitmap of the frame being drawn
	bitmap_rgb32 *m_published = nullptr;    // finished frame to show
	std::function<void ()> m_draw_ready;    // their ready callbacks
	std::function<void ()> m_published_ready;
	std::function<void ()> m_texture_ready; // for the frame in the texture, not called yet
	gpu_texture m_draw_texture;             // GPU textures of those frames
	gpu_texture m_published_texture;
	gpu_texture m_texture_texture;
	void const *m_texture_base = nullptr;   // first pixel of the frame in the texture
	void machine_exit();
	rectangle shown_area() const;           // of a frame made now
	rectangle m_draw_area;                  // a frame in the memory of a chip was made for
	rectangle m_published_area;
	bool m_exited = false;
	render_texture *m_texture_target = nullptr; // the texture
	bitmap_rgb32 *m_texture_frame = nullptr;    // the frame in it
	rectangle m_texture_area;
	int m_shown = -1;                       // own bitmap shown, or -1
	u64 m_frame = ~u64(0);
};

#endif // MAME_EMU_HIRES3D_H
