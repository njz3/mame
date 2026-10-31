// license:BSD-3-Clause
// copyright-holders:njz3
// native pixels written by the CPU (LFB) or blitted: pixel N is a block of
// scale x scale target pixels drawn by vertices 6N to 6N+5
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = VKB_PIXELS) readonly buffer pixels_t { uint pixels[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;
	int scale;
	int pad;
} pc;

layout(location = 0) flat out uint value;

const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main()
{
	uint index = uint(gl_VertexIndex) / 6u;
	uint xy = pixels[index * 2u];
	value = pixels[index * 2u + 1u];
	vec2 pos = (vec2(float(xy & 0xffffu), float(xy >> 16)) + corners[uint(gl_VertexIndex) % 6u]) * float(pc.scale);
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
