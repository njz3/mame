// license:BSD-3-Clause
// copyright-holders:njz3
// the HNG64 screen at the high resolution: the native layers under the 3D,
// the 3D blitted over them (blit_3d: scroll, vertical stretch, 3D palette
// with its 16 brightness levels, 50% blend), then the pixels the layers
// over the 3D change
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(set = 0, binding = VB_PENS0) uniform usampler2D pens0;

layout(location = 0) out vec4 out_color;

// alpha_blend_r32
uint alpha_blend_r32(uint d, uint s, uint level)
{
	uint alphad = 256u - level;
	return ((((s & 0x0000ffu) * level + (d & 0x0000ffu) * alphad) >> 8)) |
			((((s & 0x00ff00u) * level + (d & 0x00ff00u) * alphad) >> 8) & 0x00ff00u) |
			((((s & 0xff0000u) * level + (d & 0xff0000u) * alphad) >> 8) & 0xff0000u);
}

void main()
{
	int dx = int(gl_FragCoord.x);
	int x = dx + pc.left;
	int y = int(gl_FragCoord.y) + pc.top;
	int sx = x / pc.scale;
	int sy = y / pc.scale;

	// native pixels: the screen under the 3D, then the layers over it
	// (alpha 0 where they leave the pixel)
	uint offset = pc.values + pc.base + uint(((sy - pc.native_top) * pc.native_width + (sx - pc.native_left)) * 2);
	uint dest = data[offset];
	uint over = data[offset + 1u];

	if (pc.blit != 0)
	{
		int mask = pc.pens_width - 1;
		int realy = ((y - pc.vis_top) * pc.yinc) >> 16;
		uint srcpix = texelFetch(pens0, ivec2((pc.hleft + x + pc.xscroll) & mask, realy & mask), 0).r & 0xffffu;
		if ((srcpix & 0x07ffu) != 0u)
		{
			// llll appp pppp pppp: lighting, alpha, palette index
			uint color = data[pc.values + pc.palette + (((srcpix & 0xf7ffu) | pc.palbase) & 0xffffu)];
			dest = ((srcpix & 0x0800u) != 0u) ? alpha_blend_r32(dest, color, 0x80u) : color;
		}
	}
	if ((over >> 24) != 0u)
		dest = over;
	out_color = vec4(float((dest >> 16) & 0xffu), float((dest >> 8) & 0xffu), float(dest & 0xffu), 255.0) * (1.0 / 255.0);
}
