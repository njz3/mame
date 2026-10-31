// license:BSD-3-Clause
// copyright-holders:David Haywood
#ifndef MAME_KONAMI_K001005_H
#define MAME_KONAMI_K001005_H

#pragma once

#include "video/poly.h"
#include "cpu/sharc/sharc.h"
#include "k001005_vk.h"
#include "k001006.h"
#include "video/rgbutil.h"

#include <cfloat>


struct k001005_polydata
{
	int texture_x, texture_y;
	int texture_width, texture_height;
	int texture_page;
	int texture_palette;
	bool texture_mirror;
	rgb_t poly_color;
	rgb_t diffuse_light;
	rgb_t ambient_light;
	rgb_t fog_color;
	uint32_t cmd;
	bool fog_enable;
};

class k001005_renderer : public poly_manager<float, k001005_polydata, 10>
{
	friend class k001005_device;

public:
	k001005_renderer(device_t &parent, screen_device &screen, device_t *k001006, int scale);

	void reset();
	void push_data(uint32_t data);
	void swap_buffers();
	bool fifo_filled();
	void draw(bitmap_rgb32 &bitmap, const rectangle &cliprect, int scale);

	int parse_polygon(int index, uint32_t cmd);
	void render_polygons();

	// -hires3d on the GPU: the triangles are handed to it
	void set_vulkan(std::unique_ptr<k001005_vk> &&vk) { m_vk = std::move(vk); }

	template<bool UseTexture, bool UseVertexColor>
	void draw_scanline_generic(int32_t scanline, const extent_t& extent, const k001005_polydata& extradata, int threadid);

	static constexpr int POLY_Z = 0;
	static constexpr int POLY_FOG = 1;
	static constexpr int POLY_DIFF = 2;
	static constexpr int POLY_U = 3;
	static constexpr int POLY_V = 4;
	static constexpr int POLY_W = 5;
	static constexpr int POLY_R = 6;
	static constexpr int POLY_G = 7;
	static constexpr int POLY_B = 8;
	static constexpr int POLY_A = 9;

private:
	// the frame buffers are m_scale times the native 512x384 (-hires3d)
	bitmap_rgb32 m_fb[2];
	bitmap_ind32 m_zb;
	rectangle m_cliprect;
	int m_fb_page;
	int m_scale;

	std::unique_ptr<uint32_t []> m_3dfifo;
	int m_3dfifo_ptr;

	vertex_t m_vertexb[4];
	int m_vertexb_ptr = 0;

	uint32_t m_light_r;
	uint32_t m_light_g;
	uint32_t m_light_b;
	uint32_t m_ambient_r;
	uint32_t m_ambient_g;
	uint32_t m_ambient_b;
	uint32_t m_fog_r;
	uint32_t m_fog_g;
	uint32_t m_fog_b;
	float m_far_z;
	float m_fog_start_z;
	float m_fog_end_z;
	uint16_t m_reg_fog_start;

	int16_t m_viewport_min_x;
	int16_t m_viewport_max_x;
	int16_t m_viewport_min_y;
	int16_t m_viewport_max_y;
	int16_t m_viewport_center_x;
	int16_t m_viewport_center_y;

	device_t *m_k001006;

	std::unique_ptr<int[]> m_tex_mirror_table[2][8];

	std::unique_ptr<k001005_vk> m_vk;
	bool m_vk_ready = false;                // K001006 textures and palette handed over
	void emit_vk(const k001005_polydata &extra, const vertex_t &v0, const vertex_t &v1, const vertex_t &v2, const rectangle &cliprect);
};


class k001005_device : public device_t, public device_video_interface
{
public:
	k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock = 0);

	template <typename T> k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock, T &&texel_tag)
		: k001005_device(mconfig, tag, owner, clock)
	{
		set_texel_tag(std::forward<T>(texel_tag));
	}
	template <typename T> k001005_device(const machine_config &mconfig, const char *tag, device_t *owner, T &&texel_tag)
		: k001005_device(mconfig, tag, owner, 0, std::forward<T>(texel_tag))
	{
		set_texel_tag(std::forward<T>(texel_tag));
	}


	template <typename T> void set_texel_tag(T &&tag) { m_k001006.set_tag(std::forward<T>(tag)); }

	// scale is the resolution multiple of bitmap: 1 for the native screen,
	// or the -hires3d factor for a high-resolution frame
	void draw(bitmap_rgb32 &bitmap, const rectangle &cliprect, int scale = 1);
	void swap_buffers();

	// -hires3d on the GPU: the screen is composed there from the native back
	// and front layers and the 3D, and shown from the read-back memory;
	// nullptr when the 3D is drawn in software or composing fails
	bool hires_on_gpu() const;
	bitmap_rgb32 *compose_hires(bool with_3d, const bitmap_rgb32 &back, const bitmap_rgb32 &front, const rectangle &cliprect, int scale);

	uint32_t read(address_space &space, offs_t offset, uint32_t mem_mask = ~0);
	void write(address_space &space, offs_t offset, uint32_t data, uint32_t mem_mask = ~0);

protected:
	// device-level overrides
	virtual void device_start() override ATTR_COLD;
	virtual void device_stop() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

private:
	// internal state
	optional_device<k001006_device> m_k001006;

	std::unique_ptr<uint16_t[]> m_ram[2];
	std::unique_ptr<uint32_t[]> m_fifo;
	uint32_t m_status;

	int m_ram_ptr;
	int m_fifo_read_ptr;
	int m_fifo_write_ptr;

	std::unique_ptr<k001005_renderer> m_renderer;
};

DECLARE_DEVICE_TYPE(K001005, k001005_device)

#endif // MAME_KONAMI_K001005_H
