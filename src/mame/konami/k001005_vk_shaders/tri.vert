// license:BSD-3-Clause
// copyright-holders:njz3
// K001005 triangles: vertices read from the frame data; the depth is the
// polygon Z scaled by 2^-40 (exact in floating point), clamped by the GPU
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = KB_FRAME) readonly buffer frame_t { uint frame[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;
	uint vertices;
	uint palette;
} pc;

layout(location = 0) noperspective out vec4 zfd;     // z, fog, diffuse
layout(location = 1) noperspective out vec4 uvw;
layout(location = 2) noperspective out vec4 rgba;
layout(location = 3) flat out uint record;

float F(uint index) { return uintBitsToFloat(frame[index]); }

void main()
{
	uint v = pc.vertices + uint(gl_VertexIndex) * KV_SIZE;
	vec2 pos = vec2(F(v), F(v + 1u));
	float z = F(v + 2u);
	zfd = vec4(z, F(v + 3u), F(v + 4u), 0.0);
	uvw = vec4(F(v + 5u), F(v + 6u), F(v + 7u), 0.0);
	rgba = vec4(F(v + 8u), F(v + 9u), F(v + 10u), F(v + 11u));
	record = frame[v + 12u];
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, z * (1.0 / 1099511627776.0), 1.0);
}
