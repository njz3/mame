// license:BSD-3-Clause
// copyright-holders:njz3
// System 21 triangles: the box of rows and columns rendertri fills
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
			float(data[record + TR_LEFT]), float(data[record + TR_TOP]),
			float(data[record + TR_RIGHT]), float(data[record + TR_BOTTOM]));
}
