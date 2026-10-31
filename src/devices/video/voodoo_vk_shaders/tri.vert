// license:BSD-3-Clause
// copyright-holders:njz3
// Voodoo triangles: the three corners of triangle N are vertices 3N to 3N+2,
// read from its record
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = VKB_RECORDS) readonly buffer records_t { uint rec[]; };
layout(std430, set = 0, binding = VKB_OFFSETS) readonly buffer offsets_t { uint offsets[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;
	int scale;
	int pad;
} pc;

layout(location = 0) flat out uint base;

void main()
{
	uint tri = uint(gl_VertexIndex) / 3u;
	uint corner = uint(gl_VertexIndex) % 3u;
	base = offsets[tri];
	vec2 pos = vec2(uintBitsToFloat(rec[base + VKR_VX0 + corner * 2u]), uintBitsToFloat(rec[base + VKR_VX0 + corner * 2u + 1u]));
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
