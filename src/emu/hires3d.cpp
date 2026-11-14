// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    hires3d.cpp

    High-resolution output for emulated 3D hardware (mame3d).

***************************************************************************/

#include "emu.h"
#include "hires3d.h"

#include "emuopts.h"
#include "emupal.h"
#include "render.h"
#include "screen.h"
#include "vk3d.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

// outputs of the running machine, looked up by the screens
std::vector<hires3d_output *> s_outputs;

// some texture ready callback has not been called yet
bool s_reads_pending = false;

// the GPU sink of the OSD renderer
hires3d_gpu_sink *s_gpu_sink = nullptr;

} // anonymous namespace


hires3d_output::hires3d_output(screen_device &screen)
	: m_screen(screen)
	, m_scale(std::clamp(screen.machine().options().int_value(OPTION_HIRES3D), 1, 16))
{
	s_outputs.push_back(this);

	// -widescreen: 16:9 over the physical shape of the screen
	emu_options const &options = screen.machine().options();
	m_wide_option = options.bool_value(OPTION_WIDESCREEN);
	m_stretch_2d = !std::strcmp(options.value(OPTION_WIDESCREEN_2D), "stretch");
	m_wide_bg = options.bool_value(OPTION_WIDESCREEN_BG);
	std::pair<unsigned, unsigned> const aspect = screen.physical_aspect();
	if ((aspect.first > 0) && (aspect.second > 0))
		m_wide_factor = std::max((16.0f / 9.0f) / (float(aspect.first) / float(aspect.second)), 1.0f);

	// the chips free their memory after the screens and textures are gone
	screen.machine().add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(&hires3d_output::machine_exit, this));
}


void hires3d_output::machine_exit()
{
	m_exited = true;
}


hires3d_output::~hires3d_output()
{
	s_outputs.erase(std::remove(s_outputs.begin(), s_outputs.end(), this), s_outputs.end());
}


bool hires3d_output::active() const
{
	return ((m_scale > 1) || wide()) && m_screen.machine().render().is_live(m_screen);
}


int hires3d_output::pad() const
{
	return wide() ? int(std::lround(float(m_screen.visible_area().width() * m_scale) * (m_wide_factor - 1.0f) / 2.0f)) : 0;
}


float hires3d_output::view_widening()
{
	// (the windows are destroyed after the devices are stopped, and resized)
	for (hires3d_output *output : s_outputs)
	{
		if (output->m_screen.started() && output->anamorphic() && output->m_screen.machine().render().is_live(output->m_screen))
			return output->m_wide_factor;
		if (output->m_screen.started() && output->wide() && output->active())
		{
			float const width = float(output->m_screen.visible_area().width() * output->m_scale);
			return (width + 2.0f * float(output->pad())) / width;
		}
	}
	return 1.0f;
}


void hires3d_output::begin_frame()
{
	// partial updates of the same frame draw into the same bitmap; a new
	// frame never draws into the bitmap the renderer is showing
	if (m_screen.frame_number() != m_frame)
	{
		m_frame = m_screen.frame_number();
		m_current = (m_shown == 0) ? 1 : 0;
		m_draw = &m_bitmap[m_current];
		m_draw_ready = nullptr;
		m_draw_texture = gpu_texture();
	}
	if (m_draw == &m_bitmap[m_current])
	{
		int const width = m_screen.width() * m_scale + 2 * pad();
		int const height = m_screen.height() * m_scale;
		if ((m_draw->width() != width) || (m_draw->height() != height))
			m_draw->allocate(width, height);
	}
}


bitmap_rgb32 &hires3d_output::frame_bitmap()
{
	begin_frame();
	return *m_draw;
}


void hires3d_output::use_frame(bitmap_rgb32 &bitmap, std::function<void ()> &&ready, gpu_texture &&texture)
{
	begin_frame();
	m_draw = &bitmap;
	m_draw_area = shown_area();
	m_draw_ready = std::move(ready);
	m_draw_texture = std::move(texture);
}


void hires3d_output::frame_done(const rectangle &cliprect)
{
	if (m_draw && (cliprect.bottom() >= m_screen.visible_area().bottom()))
	{
		m_published = m_draw;
		m_published_area = (m_draw == &m_bitmap[m_current]) ? shown_area() : m_draw_area;
		m_published_ready = std::move(m_draw_ready);
		m_draw_ready = nullptr;
		m_published_texture = std::move(m_draw_texture);
		m_draw_texture = gpu_texture();
	}
}


rectangle hires3d_output::shown_area() const
{
	rectangle area = scaled(m_screen.visible_area());
	area.min_x -= pad();
	area.max_x += pad();
	return area;
}


rectangle hires3d_output::scaled(const rectangle &native) const
{
	int const p = pad();
	return rectangle(
			native.left() * m_scale + p, (native.right() + 1) * m_scale - 1 + p,
			native.top() * m_scale, (native.bottom() + 1) * m_scale - 1);
}


void hires3d_output::upscale(const bitmap_rgb32 &source, bitmap_rgb32 &dest, const rectangle &cliprect, bool transparent) const
{
	int const scale = m_scale;
	if (wide() && m_stretch_2d)
	{
		// stretched over the whole width of the frame
		rectangle const &visible = m_screen.visible_area();
		int const p = pad();
		int const first = visible.left() * scale;
		int const width = visible.width() * scale + 2 * p;
		int const left = first + (cliprect.left() - visible.left()) * width / visible.width();
		int const right = first + (cliprect.right() + 1 - visible.left()) * width / visible.width();
		for (int y = cliprect.top(); y <= cliprect.bottom(); y++)
		{
			u32 const *const src = &source.pix(y);
			for (int sub = 0; sub < scale; sub++)
			{
				u32 *const dst = &dest.pix(y * scale + sub);
				for (int x = left; x < right; x++)
				{
					u32 const pixel = src[visible.left() + (x - first) * visible.width() / width];
					if (!transparent || (pixel & 0xff000000))
						dst[x] = pixel;
				}
			}
		}
		return;
	}
	int const left = cliprect.left() * scale + pad();
	size_t const bytes = size_t(cliprect.width()) * scale * sizeof(u32);
	for (int y = cliprect.top(); y <= cliprect.bottom(); y++)
	{
		u32 const *const src = &source.pix(y);
		if (!transparent)
		{
			// magnify one row, then copy it
			u32 *dst = &dest.pix(y * scale, left);
			for (int x = cliprect.left(); x <= cliprect.right(); x++)
			{
				u32 const pixel = src[x];
				for (int i = 0; i < scale; i++)
					*dst++ = pixel;
			}
			u32 const *const first = &dest.pix(y * scale, left);
			for (int sub = 1; sub < scale; sub++)
				std::memcpy(&dest.pix(y * scale + sub, left), first, bytes);
		}
		else
		{
			for (int x = cliprect.left(); x <= cliprect.right(); x++)
			{
				u32 const pixel = src[x];
				if (pixel & 0xff000000)
				{
					for (int sub = 0; sub < scale; sub++)
					{
						u32 *const dst = &dest.pix(y * scale + sub, x * scale + left - cliprect.left() * scale);
						for (int i = 0; i < scale; i++)
							dst[i] = pixel;
					}
				}
			}
		}
	}
}


void hires3d_output::magnify_changes(const bitmap_rgb32 &before, const bitmap_rgb32 &after, const rectangle &cliprect)
{
	int const scale = m_scale;
	bitmap_rgb32 &frame = frame_bitmap();
	for (int y = cliprect.top(); y <= cliprect.bottom(); y++)
	{
		u32 const *const old = &before.pix(y);
		u32 const *const now = &after.pix(y);
		for (int x = cliprect.left(); x <= cliprect.right(); x++)
		{
			if (now[x] != old[x])
			{
				for (int sub = 0; sub < scale; sub++)
					std::fill_n(&frame.pix(y * scale + sub, x * scale), scale, now[x]);
			}
		}
	}
}


void hires3d_output::upscale(const bitmap_ind16 &source, bitmap_ind16 &dest, const rectangle &cliprect, int transparent_pen) const
{
	int const scale = m_scale;
	int const left = cliprect.left() * scale;
	size_t const bytes = size_t(cliprect.width()) * scale * sizeof(u16);
	for (int y = cliprect.top(); y <= cliprect.bottom(); y++)
	{
		u16 const *const src = &source.pix(y);
		if (transparent_pen < 0)
		{
			// magnify one row, then copy it
			u16 *dst = &dest.pix(y * scale, left);
			for (int x = cliprect.left(); x <= cliprect.right(); x++)
			{
				u16 const pixel = src[x];
				for (int i = 0; i < scale; i++)
					*dst++ = pixel;
			}
			u16 const *const first = &dest.pix(y * scale, left);
			for (int sub = 1; sub < scale; sub++)
				std::memcpy(&dest.pix(y * scale + sub, left), first, bytes);
		}
		else
		{
			for (int x = cliprect.left(); x <= cliprect.right(); x++)
			{
				u16 const pixel = src[x];
				if (pixel != transparent_pen)
				{
					for (int sub = 0; sub < scale; sub++)
					{
						u16 *const dst = &dest.pix(y * scale + sub, x * scale);
						for (int i = 0; i < scale; i++)
							dst[i] = pixel;
					}
				}
			}
		}
	}
}


bool hires3d_output::narrow_params(float &middle, float &factor, int &first, int &last) const
{
	rectangle const &visible = m_screen.visible_area();
	first = (m_narrow_first >= 0) ? m_narrow_first : visible.left();
	last = (m_narrow_last >= 0) ? m_narrow_last : visible.right();
	middle = (first + last + 1) * 0.5f;
	factor = m_wide_factor;
	return narrow_2d();
}


int hires3d_output::narrow_column(int x, int scale) const
{
	float middle, factor;
	int first, last;
	narrow_params(middle, factor, first, last);
	int from = x / scale;
	if ((from >= first) && (from <= last))
	{
		from = int(std::floor(middle + ((x + 0.5f) / scale - middle) * factor));
		if ((from < first) || (from > last))
			return -1;
	}
	return from;
}


template <typename T, typename Opaque>
void hires3d_output::narrow_pixels(const T &source, T &dest, const rectangle &cliprect, int scale, Opaque &&opaque) const
{
	// destination column x (scale per native column) shows the native
	// column at the same distance from the middle, wide_factor() times
	// further, so the 2D keeps the sharpness of the magnification; the
	// columns outside the narrowed area stay in place
	int const left = cliprect.left() * scale;
	int const right = (cliprect.right() + 1) * scale;
	for (int y = cliprect.top(); y <= cliprect.bottom(); y++)
	{
		auto const *const src = &source.pix(y);
		for (int x = left; x < right; x++)
		{
			int const from = narrow_column(x, scale);
			if (from < 0)
				continue;
			auto const pixel = src[from];
			if (opaque(pixel))
				for (int sub = 0; sub < scale; sub++)
					dest.pix(y * scale + sub, x) = pixel;
		}
	}
}


void hires3d_output::narrow(const bitmap_ind16 &source, bitmap_ind16 &dest, const rectangle &cliprect, u16 transparent_pen) const
{
	narrow_pixels(source, dest, cliprect, 1, [transparent_pen] (u16 pixel) { return pixel != transparent_pen; });
}


void hires3d_output::narrow(const bitmap_rgb32 &source, bitmap_rgb32 &dest, const rectangle &cliprect) const
{
	narrow_pixels(source, dest, cliprect, 1, [] (u32 pixel) { return (pixel & 0xff000000) != 0; });
}


void hires3d_output::draw_2d(bitmap_rgb32 &bitmap, const rectangle &cliprect, bitmap_rgb32 &temp, const std::function<void (bitmap_rgb32 &)> &draw) const
{
	if (!narrow_2d())
	{
		draw(bitmap);
		return;
	}
	if ((temp.width() != bitmap.width()) || (temp.height() != bitmap.height()))
		temp.allocate(bitmap.width(), bitmap.height());
	temp.fill(0, cliprect);
	draw(temp);
	narrow(temp, bitmap, cliprect);
}


void hires3d_output::upscale_2d(const bitmap_ind16 &source, bitmap_ind16 &dest, const rectangle &cliprect, u16 transparent_pen) const
{
	if (narrow_2d())
		narrow_pixels(source, dest, cliprect, m_scale, [transparent_pen] (u16 pixel) { return pixel != transparent_pen; });
	else
		upscale(source, dest, cliprect, transparent_pen);
}


void hires3d_output::upscale_2d(const bitmap_rgb32 &source, bitmap_rgb32 &dest, const rectangle &cliprect) const
{
	if (narrow_2d())
		narrow_pixels(source, dest, cliprect, m_scale, [] (u32 pixel) { return (pixel & 0xff000000) != 0; });
	else
		upscale(source, dest, cliprect, true);
}


void hires3d_output::indexed_to_frame(const bitmap_ind16 &source, const rectangle &hclip, device_palette_interface &palette)
{
	// an indexed screen gets its colours when it is displayed: the pens of
	// such a palette are pen numbers, the colours are the adjusted entries
	bitmap_rgb32 &dest = frame_bitmap();
	rgb_t const *const colors = palette.palette()->entry_list_adjusted();
	u32 const count = palette.entries();
	for (int y = hclip.top(); y <= hclip.bottom(); y++)
	{
		u16 const *const src = &source.pix(y);
		u32 *const dst = &dest.pix(y);
		for (int x = hclip.left(); x <= hclip.right(); x++)
			dst[x] = colors[(src[x] < count) ? src[x] : 0];
	}
}


vk3d_renderer *hires3d_output::vulkan()
{
	if (!m_vulkan_tried)
	{
		m_vulkan_tried = true;
		char const *const renderer = m_screen.machine().options().value(OPTION_HIRES3D_RENDERER);
		if (std::strcmp(renderer, "software"))
		{
			std::string error;
			m_vulkan = vk3d_renderer::create(error);
			if (m_vulkan)
				osd_printf_verbose("hires3d: Vulkan rendering on %s\n", m_vulkan->device_name());
			else
				osd_printf_warning("hires3d: Vulkan not available (%s), using software rendering\n", error);
		}
	}
	return m_vulkan.get();
}


void hires3d_output::disable_vulkan(const char *reason)
{
	osd_printf_warning("hires3d: Vulkan rendering stopped (%s), using software rendering\n", reason);
	m_vulkan.reset();
}


bool hires3d_output::update_texture(screen_device &screen, render_texture &texture)
{
	for (hires3d_output *output : s_outputs)
	{
		if ((&output->m_screen == &screen) && output->m_published)
		{
			// the frame the renderer did not read is finished first; the
			// new one is waited for when the renderer reads it
			if (output->m_texture_ready)
			{
				std::function<void ()> const ready = std::move(output->m_texture_ready);
				output->m_texture_ready = nullptr;
				ready();
			}
			output->m_texture_ready = std::move(output->m_published_ready);
			output->m_published_ready = nullptr;
			if (output->m_texture_ready)
				s_reads_pending = true;
			output->m_texture_texture = std::move(output->m_published_texture);
			output->m_published_texture = gpu_texture();
			// the area the frame was made for: a frame in the memory of a chip
			// does not cover another one if the visible area changed since
			rectangle const area = output->m_published_area;
			output->m_texture_base = &output->m_published->pix(area.top(), area.left());
			texture.set_bitmap(*output->m_published, area, TEXFORMAT_RGB32);
			output->m_texture_target = &texture;
			output->m_texture_frame = output->m_published;
			output->m_texture_area = area;
			output->m_shown = (output->m_published == &output->m_bitmap[0]) ? 0 : (output->m_published == &output->m_bitmap[1]) ? 1 : -1;
			output->m_published = nullptr;
			return true;
		}
	}
	return false;
}


void hires3d_output::frames_freed(void const *start, size_t bytes)
{
	u8 const *const first = static_cast<u8 const *>(start);
	auto const inside = [first, bytes] (void const *pixel) { return (static_cast<u8 const *>(pixel) >= first) && (static_cast<u8 const *>(pixel) < first + bytes); };
	for (hires3d_output *output : s_outputs)
	{
		if (output->m_exited)
			continue;

		// not given to the renderer yet: the frame shown stays
		if (output->m_published && inside(&output->m_published->pix(output->m_published_area.top(), output->m_published_area.left())))
		{
			output->m_published = nullptr;
			output->m_published_ready = nullptr;
			output->m_published_texture = gpu_texture();
		}
		if ((output->m_draw != nullptr) && (output->m_draw != &output->m_bitmap[output->m_current]) && inside(&output->m_draw->pix(output->m_draw_area.top(), output->m_draw_area.left())))
		{
			output->m_draw = &output->m_bitmap[output->m_current];
			output->m_draw_ready = nullptr;
			output->m_draw_texture = gpu_texture();
		}

		// shown by the renderer, and drawn again until the next frame: from a
		// copy, in the own bitmap that is not being drawn
		if ((output->m_shown < 0) && output->m_texture_target && output->m_texture_frame && inside(output->m_texture_base))
		{
			rectangle const &shown = output->m_texture_area;
			int const own = (output->m_current == 0) ? 1 : 0;
			bitmap_rgb32 &copy = output->m_bitmap[own];
			if ((copy.width() <= shown.right()) || (copy.height() <= shown.bottom()))
				copy.allocate(shown.right() + 1, shown.bottom() + 1);
			for (int y = shown.top(); y <= shown.bottom(); y++)
				std::copy_n(&output->m_texture_frame->pix(y, shown.left()), shown.width(), &copy.pix(y, shown.left()));
			output->m_texture_target->set_bitmap(copy, shown, TEXFORMAT_RGB32);
			output->m_texture_base = &copy.pix(shown.top(), shown.left());
			output->m_texture_frame = &copy;
			output->m_texture_ready = nullptr;
			output->m_texture_texture = gpu_texture();
			output->m_shown = own;
		}
	}
}


void hires3d_output::set_gpu_sink(hires3d_gpu_sink *sink)
{
	s_gpu_sink = sink;
	if (!sink)
	{
		// the textures are gone: the frames shown from them are lost
		for (hires3d_output *output : s_outputs)
			output->m_texture_texture = output->m_published_texture = output->m_draw_texture = gpu_texture();
	}
}


hires3d_gpu_sink *hires3d_output::gpu_sink()
{
	return s_gpu_sink;
}


int hires3d_output::gpu_texture_of(void const *base)
{
	for (hires3d_output *output : s_outputs)
		if (output->m_texture_texture.id && (output->m_texture_base == base))
			return output->m_texture_texture.id;
	return 0;
}


bool hires3d_output::gpu_texture_in_use(int id)
{
	for (hires3d_output *output : s_outputs)
		if ((output->m_texture_texture.id == id) || (output->m_published_texture.id == id) || (output->m_draw_texture.id == id))
			return true;
	return false;
}


void hires3d_output::read_gpu_textures()
{
	// the frames are read once their GPU work is done; each is then in its
	// bitmap and drawn from there
	complete_reads();
	for (hires3d_output *output : s_outputs)
	{
		if (output->m_texture_texture.id && output->m_texture_texture.read)
			output->m_texture_texture.read();
		output->m_texture_texture = gpu_texture();
	}
}


void hires3d_output::read_gpu_texture(void const *base)
{
	for (hires3d_output *output : s_outputs)
		if (output->m_texture_texture.id && (output->m_texture_base == base))
		{
			complete_reads();
			if (output->m_texture_texture.read)
				output->m_texture_texture.read();
			output->m_texture_texture = gpu_texture();
		}
}


void hires3d_output::complete_reads()
{
	if (!s_reads_pending)
		return;
	s_reads_pending = false;
	for (hires3d_output *output : s_outputs)
	{
		if (output->m_texture_ready)
		{
			std::function<void ()> const ready = std::move(output->m_texture_ready);
			output->m_texture_ready = nullptr;
			ready();
		}
	}
}
