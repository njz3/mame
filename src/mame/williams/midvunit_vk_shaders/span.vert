// license:BSD-3-Clause
// copyright-holders:njz3
// CPU writes into video RAM: a run of native pixels in a row, each a block
// of scale x scale pixels
#version 450
#extension GL_GOOGLE_include_directive : require

#define VERTEX_SHADER
#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint values;
layout(location = 1) flat out int x0;

void main()
{
	uint span = pc.base + (uint(gl_VertexIndex) / 6u) * SR_SIZE;
	int x = int(data[span + SR_X]);
	int y = int(data[span + SR_Y]);
	int count = int(data[span + SR_COUNT]);
	values = pc.values + data[span + SR_VALUES];
	x0 = x;
	gl_Position = box_position(float(x * pc.scale), float(y * pc.scale), float((x + count) * pc.scale), float((y + 1) * pc.scale));
}
