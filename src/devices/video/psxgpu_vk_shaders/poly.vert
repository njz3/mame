// license:BSD-3-Clause
// copyright-holders:njz3
// PlayStation triangles: colours and texture coordinates are affine
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = 0) readonly buffer data_t { uint data[]; };
layout(push_constant) uniform constants_t { vec2 inv_size; float scale; int pass; uint records; } pc;

layout(location = 0) flat out uint record;
layout(location = 1) noperspective out vec3 colour;
layout(location = 2) noperspective out vec2 uv;

float F(uint offset) { return uintBitsToFloat(data[offset]); }

void main()
{
	uint index = uint(gl_VertexIndex);
	record = pc.records + (index / 3u) * uint(TR_SIZE);
	uint vert = record + uint(TR_VERTS) + (index % 3u) * uint(TV_SIZE);
	colour = vec3(F(vert + uint(TV_R)), F(vert + uint(TV_G)), F(vert + uint(TV_B)));
	uv = vec2(F(vert + uint(TV_U)), F(vert + uint(TV_V)));
	vec2 pos = vec2(F(vert + uint(TV_X)), F(vert + uint(TV_Y))) * pc.scale;
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
