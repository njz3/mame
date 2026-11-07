// license:BSD-3-Clause
// copyright-holders:njz3
// the HNG64 screen at the high resolution: the native layers under the 3D,
// the 3D blitted over them (blit_3d: scroll, vertical stretch, 3D palette
// with its 16 brightness levels, 50% blend), then the layers over the 3D;
// with -widescreen, the frame has pad more columns on each side: the 3D
// buffer too, the layers over it are in the middle or stretched, and the
// layers under it are bg_pad native columns wider on each side, else as the
// layers over it
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

int floor_div(int a, int b)
{
	return (a >= 0) ? (a / b) : -((b - 1 - a) / b);
}

void main()
{
	int dx = int(gl_FragCoord.x);
	int x = dx + pc.left - pc.pad;      // high-resolution screen pixel
	int y = int(gl_FragCoord.y) + pc.top;
	int sy = y / pc.scale;

	// native pixels: the screen under the 3D, then the layers over it
	// (alpha 0xff when opaque, else added to alpha / 128 times what is under)
	int tx = (pc.stretch2d != 0)
			? (pc.native_left + int(floor((float(dx) + 0.5) * float(pc.native_width) / float(pc.frame_width))))
			: floor_div(x, pc.scale);
	int bx = (pc.bg_pad > 0) ? floor_div(x, pc.scale) : tx;
	int stride = pc.native_width + 2 * pc.bg_pad;
	uint row = pc.values + pc.base + uint((sy - pc.native_top) * stride * 2);
	int tcol = tx - pc.native_left;
	int bcol = bx - pc.native_left + pc.bg_pad;
	uint dest = ((bcol >= 0) && (bcol < stride)) ? data[row + uint(bcol * 2)] : 0u;
	uint over = ((tcol >= 0) && (tcol < pc.native_width)) ? data[row + uint((tcol + pc.bg_pad) * 2 + 1)] : 0x80000000u;

	if (pc.blit != 0)
	{
		// the 3D buffer wraps around, past its widescreen sides
		int fbw = pc.pens_width - 2 * pc.pad;
		int fx = pc.hleft + x + pc.xscroll;
		if ((fx < -pc.pad) || (fx >= fbw + pc.pad))
			fx = ((fx % fbw) + fbw) % fbw;
		int realy = ((y - pc.vis_top) * pc.yinc) >> 16;
		int fy = ((realy % pc.pens_height) + pc.pens_height) % pc.pens_height;
		uint srcpix = texelFetch(pens0, ivec2(fx + pc.pad, fy), 0).r & 0xffffu;
		if ((srcpix & 0x07ffu) != 0u)
		{
			// llll appp pppp pppp: lighting, alpha, palette index
			uint color = data[pc.values + pc.palette + (((srcpix & 0xf7ffu) | pc.palbase) & 0xffffu)];
			dest = ((srcpix & 0x0800u) != 0u) ? alpha_blend_r32(dest, color, 0x80u) : color;
		}
	}
	uint coverage = over >> 24;
	if (coverage == 0xffu)
	{
		dest = over;
	}
	else
	{
		uvec3 under = (uvec3((dest >> 16) & 0xffu, (dest >> 8) & 0xffu, dest & 0xffu) * coverage) >> 7u;
		uvec3 result = min(under + uvec3((over >> 16) & 0xffu, (over >> 8) & 0xffu, over & 0xffu), uvec3(0xffu));
		dest = (result.r << 16) | (result.g << 8) | result.b;
	}
	out_color = vec4(float((dest >> 16) & 0xffu), float((dest >> 8) & 0xffu), float(dest & 0xffu), 255.0) * (1.0 / 255.0);
}
