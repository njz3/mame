// license:BSD-3-Clause
// copyright-holders:njz3
// the Taito JC screen at the high resolution, through the palette: the
// native layers under the 3D, the 3D, then the native layers over it
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(set = 0, binding = VB_PENS0) uniform usampler2D pens0;
layout(set = 0, binding = VB_PENS1) uniform usampler2D pens1;

layout(location = 0) out vec4 out_color;

void main()
{
	int x = int(gl_FragCoord.x) + pc.left;
	int y = int(gl_FragCoord.y) + pc.top;
	int sx = x / pc.scale;
	int sy = y / pc.scale;

	// low half: the native layers under the 3D; high half: the layers over
	// it, 0xffff where they leave the pixel
	uint native = data[pc.values + pc.base + uint((sy - pc.native_top) * pc.native_width + (sx - pc.native_left))];
	uint dest = native >> 16;
	if (dest == 0xffffu)
	{
		dest = 0u;
		if ((x < pc.pens_width) && (y < pc.pens_height))
			dest = ((pc.pens == 0) ? texelFetch(pens0, ivec2(x, y), 0).r : texelFetch(pens1, ivec2(x, y), 0).r) & 0xffffu;
		if (dest == 0u)
			dest = native & 0xffffu;
	}
	uint color = data[pc.values + pc.palette + ((dest < pc.colors) ? dest : 0u)];
	out_color = vec4(float((color >> 16) & 0xffu), float((color >> 8) & 0xffu), float(color & 0xffu), 255.0) * (1.0 / 255.0);
}
