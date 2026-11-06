// license:BSD-3-Clause
// copyright-holders:njz3
// Zeus 2 triangles, as the triangles themselves, in the render window
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;

void main()
{
	uint index = uint(gl_VertexIndex);
	record = pc.base + (index / 3u) * TR_SIZE;
	uint vert = record + TR_V1X + (index % 3u) * 2u;
	vec2 window = vec2(float(int(data[record + TR_WINX])), float(int(data[record + TR_WINY])));
	vec2 pos = window + vec2(uintBitsToFloat(data[vert]), uintBitsToFloat(data[vert + 1u]));
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
