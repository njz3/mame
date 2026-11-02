// license:BSD-3-Clause
// copyright-holders:njz3
// V-Unit quads: the box of rows and columns the quad may cover; the
// fragment shader decides which pixels it draws
#version 450
#extension GL_GOOGLE_include_directive : require

#define VERTEX_SHADER
#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;

void main()
{
	record = pc.base + (uint(gl_VertexIndex) / 6u) * QR_SIZE;
	gl_Position = box_position(
			float(int(data[record + QR_BOXLEFT])), float(int(data[record + QR_TOP])),
			float(int(data[record + QR_BOXRIGHT])), float(int(data[record + QR_BOTTOM])));
}
