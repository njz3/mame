// license:BSD-3-Clause
// copyright-holders:njz3
// Gaelco 3D triangles, as the triangles themselves, rows upside down in the
// image
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;

void main()
{
	uint index = uint(gl_VertexIndex);
	record = pc.base + (index / 3u) * GR_SIZE;
	uint vert = record + GR_VERTS + (index % 3u) * 2u;
	vec2 pos = vec2(uintBitsToFloat(data[vert]), float(pc.height) - uintBitsToFloat(data[vert + 1u]));
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
