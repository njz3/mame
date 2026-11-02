// license:BSD-3-Clause
// copyright-holders:njz3
// TC0780FPA triangles and quads: the box of rows and columns the primitive
// may cover; the fragment shader decides which pixels it draws
#version 450
#extension GL_GOOGLE_include_directive : require

#define VERTEX_SHADER
#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;

void main()
{
	record = pc.base + (uint(gl_VertexIndex) / 6u) * uint((pc.kind == KIND_QUAD) ? QR_SIZE : TR_SIZE);
	gl_Position = box_position(
			float(int(data[record + PR_BOXLEFT])), float(int(data[record + PR_TOP])),
			float(int(data[record + PR_BOXRIGHT])), float(int(data[record + PR_BOTTOM])));
}
