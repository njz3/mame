// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    vk3d.h

    Off-screen Vulkan renderer for high-resolution 3D (mame3d).

    A driver records a frame as batches of triangles in target pixel
    coordinates, then end_frame() renders it on the GPU and reads the
    image back into a bitmap that is shown through hires3d_output.  The
    Vulkan loader is opened at run time: without Vulkan, create() fails
    and the driver keeps its software renderer.

    Pixels use the bitmap_rgb32 layout (B8G8R8A8 in memory) for the
    target and the textures, so no conversion is needed either way.

***************************************************************************/

#ifndef MAME_EMU_VK3D_H
#define MAME_EMU_VK3D_H

#pragma once

#include <memory>
#include <string>

#define OPTION_HIRES3D_RENDERER "hires3d_renderer"


class vk3d_renderer
{
public:
	// one vertex: position in target pixels (z for depth tests, w as the
	// clip-space w for perspective-correct interpolation), colour 0-1,
	// texture coordinates 0-1 in u/v plus two free parameters
	struct vertex
	{
		float x, y, z, w;
		float r, g, b, a;
		float u, v, p, q;
	};

	// built-in pipelines; drivers can add their own with add_pipeline()
	enum
	{
		PIPE_COLOR = 0,     // vertex colour
		PIPE_COLOR_STIPPLE, // vertex colour on every other checkerboard cell (cell size in p)
		PIPE_TEXTURE_KEY,   // texture modulated by colour, alpha 0 discarded
		PIPE_BUILTIN_COUNT
	};

	enum blend_mode { BLEND_NONE, BLEND_ALPHA, BLEND_ADD };

	struct pipeline_desc
	{
		u32 const *vertex_spirv = nullptr;   // nullptr: the shared vertex shader
		size_t vertex_size = 0;              // bytes
		u32 const *fragment_spirv = nullptr;
		size_t fragment_size = 0;            // bytes
		blend_mode blend = BLEND_NONE;
		bool depth_test = false;             // less-or-equal
		bool depth_write = false;
	};

	static std::unique_ptr<vk3d_renderer> create(std::string &error);
	~vk3d_renderer();

	std::string const &device_name() const;

	// pipelines and textures stay valid until the renderer is destroyed
	int add_pipeline(pipeline_desc const &desc);
	int create_texture(int width, int height, bool linear_filter = false);
	void update_texture(int texture, bitmap_rgb32 const &source, rectangle const &area);

	// record a frame of the given size; draws are clipped to the scissor
	void begin_frame(int width, int height, rgb_t clear);
	void set_scissor(rectangle const &clip);
	void draw(int pipeline, int texture, vertex const *vertices, size_t count); // triangle list
	void draw_rect(int pipeline, int texture, float x0, float y0, float x1, float y1, rgb_t color);

	// render on the GPU and copy the clip area of the result into dest
	bool end_frame(bitmap_rgb32 &dest, rectangle const &clip);

private:
	struct impl;

	vk3d_renderer(std::unique_ptr<impl> &&state);

	std::unique_ptr<impl> m;
};

#endif // MAME_EMU_VK3D_H
