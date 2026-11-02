// license:BSD-3-Clause
// copyright-holders:njz3
// HNG64 triangles: the box of rows and columns the triangle may cover; the
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
	gl_Position = box_position(
			float(int(data[record + TR_BOXLEFT])), float(int(data[record + TR_TOP])),
			float(int(data[record + TR_BOXRIGHT])), float(int(data[record + TR_BOTTOM])));
}
