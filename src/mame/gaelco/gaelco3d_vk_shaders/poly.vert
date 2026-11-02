// license:BSD-3-Clause
// copyright-holders:njz3
// Gaelco 3D triangles: the box of rows and columns the triangle may cover,
// rows upside down in the image
#version 450
#extension GL_GOOGLE_include_directive : require

#define VERTEX_SHADER
#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;

void main()
{
	record = pc.base + (uint(gl_VertexIndex) / 6u) * GR_SIZE;
	float top = float(pc.height - int(data[record + GR_BOTTOM]));
	float bottom = float(pc.height - int(data[record + GR_TOP]));
	gl_Position = box_position(float(int(data[record + GR_BOXLEFT])), top, float(int(data[record + GR_BOXRIGHT])), bottom);
}
