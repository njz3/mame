// license:BSD-3-Clause
// copyright-holders:njz3
// System 23 triangles, in the order of the sorted polygons: no depth test,
// the parameters are interpolated as the scanline renderer does
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;
layout(location = 1) noperspective out vec4 params;     // z, u, v, i

float F(uint offset) { return uintBitsToFloat(data[offset]); }

void main()
{
	uint index = uint(gl_VertexIndex);
	record = (index / 3u) * uint(TR_SIZE);
	uint vert = record + uint(TR_VERTS) + (index % 3u) * uint(TV_SIZE);
	params = vec4(F(vert + uint(TV_P)), F(vert + uint(TV_P + 1)), F(vert + uint(TV_P + 2)), F(vert + uint(TV_P + 3)));
	vec2 pos = vec2(F(vert + uint(TV_X)), F(vert + uint(TV_Y)));
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
