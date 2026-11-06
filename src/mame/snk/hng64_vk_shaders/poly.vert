// license:BSD-3-Clause
// copyright-holders:njz3
// HNG64 triangles, as the triangles themselves: the GPU rasterises them,
// interpolates their parameters and tests their depth, stored as Z / 128,
// which keeps its order and the clear value of 100
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;
layout(location = 1) noperspective out vec4 params;     // W, light, S, T

float F(uint offset) { return uintBitsToFloat(data[offset]); }

void main()
{
	uint index = uint(gl_VertexIndex);
	record = pc.base + (index / 3u) * uint(TR_SIZE);
	uint vert = record + uint(TR_VERTS) + (index % 3u) * uint(TV_SIZE);
	params = vec4(F(vert + uint(TV_P + 1)), F(vert + uint(TV_P + 2)), F(vert + uint(TV_P + 3)), F(vert + uint(TV_P + 4)));
	vec2 pos = vec2(F(vert + uint(TV_X)), F(vert + uint(TV_Y)));
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, F(vert + uint(TV_P)) * (1.0 / 128.0), 1.0);
}
