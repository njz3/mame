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
#include <cstring>
#include <vector>


namespace {

// outputs of the running machine, looked up by the screens
std::vector<hires3d_output *> s_outputs;

} // anonymous namespace


hires3d_output::hires3d_output(screen_device &screen)
	: m_screen(screen)
	, m_scale(std::clamp(screen.machine().options().int_value(OPTION_HIRES3D), 1, 16))
{
	s_outputs.push_back(this);
}


hires3d_output::~hires3d_output()
{
	s_outputs.erase(std::remove(s_outputs.begin(), s_outputs.end(), this), s_outputs.end());
}


bool hires3d_output::active() const
{
	return (m_scale > 1) && m_screen.machine().render().is_live(m_screen);
}


bitmap_rgb32 &hires3d_output::frame_bitmap()
{
	// partial updates of the same frame draw into the same bitmap; a new
	// frame never draws into the bitmap the renderer is showing
	if (m_screen.frame_number() != m_frame)
	{
		m_frame = m_screen.frame_number();
		m_current = (m_shown == 0) ? 1 : 0;
	}

	bitmap_rgb32 &bitmap = m_bitmap[m_current];
	int const width = m_screen.width() * m_scale;
	int const height = m_screen.height() * m_scale;
	if ((bitmap.width() != width) || (bitmap.height() != height))
		bitmap.allocate(width, height);
	return bitmap;
}


void hires3d_output::frame_done(const rectangle &cliprect)
{
	if (cliprect.bottom() >= m_screen.visible_area().bottom())
		m_published = m_current;
}


rectangle hires3d_output::scaled(const rectangle &native) const
{
	return rectangle(
			native.left() * m_scale, (native.right() + 1) * m_scale - 1,
			native.top() * m_scale, (native.bottom() + 1) * m_scale - 1);
}


void hires3d_output::upscale(const bitmap_rgb32 &source, bitmap_rgb32 &dest, const rectangle &cliprect, bool transparent) const
{
	int const scale = m_scale;
	int const left = cliprect.left() * scale;
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
						u32 *const dst = &dest.pix(y * scale + sub, x * scale);
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
		if ((&output->m_screen == &screen) && (output->m_published >= 0))
		{
			texture.set_bitmap(output->m_bitmap[output->m_published], output->scaled(screen.visible_area()), TEXFORMAT_RGB32);
			output->m_shown = output->m_published;
			output->m_published = -1;
			return true;
		}
	}
	return false;
}
