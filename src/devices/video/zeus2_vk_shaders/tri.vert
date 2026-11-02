// license:BSD-3-Clause
// copyright-holders:njz3
// Zeus 2 triangles: the box of rows and columns the triangle may cover; the
// fragment shader decides which pixels it draws
#version 450
#extension GL_GOOGLE_include_directive : require

#define VERTEX_SHADER
#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;

void main()
{
	record = pc.base + (uint(gl_VertexIndex) / 6u) * TR_SIZE;
	float winx = float(int(data[record + TR_WINX]));
	float winy = float(int(data[record + TR_WINY]));
	gl_Position = box_position(
			winx + float(int(data[record + TR_BOXLEFT])), winy + float(int(data[record + TR_TOP])),
			winx + float(int(data[record + TR_BOXRIGHT])), winy + float(int(data[record + TR_BOTTOM])));
}
