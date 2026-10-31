// license:BSD-3-Clause
// copyright-holders:njz3
// the screen as the drivers build it: the native back layer, the opaque
// pixels of the 3D page over it, the opaque pixels of the native front
// layer over both
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = KB_FRAME) readonly buffer frame_t { uint frame[]; };
layout(set = 1, binding = 0) uniform sampler2D page;

layout(push_constant) uniform push_t
{
	uint back;          // native layers, ARGB, native_width per row
	uint front;
	int native_width;
	int scale;
	int with_3d;
} pc;

layout(location = 0) out vec4 out_color;

vec4 color_of(uint v) { return vec4(float((v >> 16) & 0xffu), float((v >> 8) & 0xffu), float(v & 0xffu), float(v >> 24)) * (1.0 / 255.0); }

void main()
{
	ivec2 xy = ivec2(gl_FragCoord.xy);
	ivec2 n = xy / pc.scale;
	uint index = uint(n.y * pc.native_width + n.x);
	uint front = frame[pc.front + index];
	if ((front >> 24) != 0u)
	{
		out_color = color_of(front);
		return;
	}
	if (pc.with_3d != 0)
	{
		vec4 c = texelFetch(page, xy, 0);
		if (c.a > 0.0)
		{
			out_color = c;
			return;
		}
	}
	out_color = color_of(frame[pc.back + index]);
}
