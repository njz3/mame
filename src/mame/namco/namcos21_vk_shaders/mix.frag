// license:BSD-3-Clause
// copyright-holders:njz3
// the screen at the high resolution, through the palette: Winning Run's
// fill, 2D layer (bitmap_draw) and 3D by priority, or Driver's Eyes' native
// sprites under and over the 3D
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_LAYER) readonly buffer layer_t { uint layer[]; };
layout(set = 0, binding = VB_PENS0) uniform usampler2D pens0;
layout(set = 0, binding = VB_PENS1) uniform usampler2D pens1;

layout(location = 0) out vec4 out_color;

// the 3D pen of a high-resolution screen pixel, 0 outside the frame buffer
uint pen_3d(int x, int y)
{
	if ((x >= pc.pens_width) || (y >= pc.pens_height))
		return 0u;
	return ((pc.pens == 0) ? texelFetch(pens0, ivec2(x, y), 0).r : texelFetch(pens1, ivec2(x, y), 0).r) & 0xffffu;
}

// namcos21_state::bitmap_draw for one pixel
uint draw_layer(uint dest, int sx, int sy)
{
	uint offset = uint(((pc.yscroll + sy) & 0x3ff) * 0x200 + ((sx + pc.xscroll) & 0x1ff));
	uint pen = (layer[offset >> 2] >> ((offset & 3u) * 8u)) & 0xffu;
	uint base = pc.layer_base;
	if (pen == 0xffu)
		return dest;
	if (pen == 0x00u)
		return (dest != (base ^ 0x10ffu)) ? (0x4000u | (dest & 0x1fffu)) : ((base & 0xf00u) | 0x00u);
	if (pen == 0x01u)
		return (dest != (base ^ 0x10ffu)) ? (0x6000u | (dest & 0x1fffu)) : ((base & 0xf00u) | 0x01u);
	return base | pen;
}

void main()
{
	int x = int(gl_FragCoord.x) + pc.left;
	int y = int(gl_FragCoord.y) + pc.top;
	int sx = x / pc.scale;
	int sy = y / pc.scale;
	uint dest;
	if (pc.mode == MIX_LAYER)
	{
		dest = pc.fill;
		if (pc.pri == 5u)
		{
			dest = draw_layer(dest, sx, sy);
			uint pen = pen_3d(x, y);
			if (pen != 0u)
				dest = pen;
		}
		else if (pc.pri == 0u)
		{
			dest = draw_layer(dest, sx, sy);
		}
		else
		{
			uint pen = pen_3d(x, y);
			if (pen != 0u)
				dest = pen;
			dest = draw_layer(dest, sx, sy);
		}
	}
	else
	{
		// low half: the native screen under the 3D; high half: the top
		// sprites' pixel, 0xffff where they leave it
		uint native = data[pc.values + pc.base + uint((sy - pc.native_top) * pc.native_width + (sx - pc.native_left))];
		dest = native >> 16;
		if (dest >= 0x8000u)
		{
			uint under = pen_3d(x, y);
			if (under == 0u)
				under = native & 0xffffu;
			if (dest == 0xffffu)
			{
				dest = under;
			}
			else
			{
				// sprite_mix_callback's pens that depend on what is under them
				uint op = dest & 3u;
				if (op == 2u)
					dest = (under == 0xffu) ? ((dest & 0xf00u) | 0xffu) : under;
				else if (under != 0xffu)
					dest = ((op == 1u) ? 0x6000u : 0x4000u) | (under & 0x1fffu);
				else
					dest = (dest & 0xf00u) | op;
			}
		}
	}
	uint color = data[pc.values + pc.palette + ((dest < pc.colors) ? dest : 0u)];
	out_color = vec4(float((color >> 16) & 0xffu), float((color >> 8) & 0xffu), float(color & 0xffu), 255.0) * (1.0 / 255.0);
}
