// license:BSD-3-Clause
// copyright-holders:njz3
// frame buffer writes: a run of native pixels in a row, each a block of
// scale x scale pixels (-widescreen: pad columns in; the fills reaching the
// ends of the row also clear its sides)
#version 450
#extension GL_GOOGLE_include_directive : require

#define VERTEX_SHADER
#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint values;
layout(location = 1) flat out int x0;
layout(location = 2) flat out uint flags;

void main()
{
	uint span = pc.base + (uint(gl_VertexIndex) / 6u) * SR_SIZE;
	int x = int(data[span + SR_X]);
	int y = int(data[span + SR_Y]);
	int count = int(data[span + SR_COUNT]);
	values = pc.values + data[span + SR_VALUES];
	x0 = x;
	flags = data[span + SR_FLAGS];
	float left = float(x * pc.scale + pc.pad);
	float right = float((x + count) * pc.scale + pc.pad);
	if ((flags & SF_CONSTANT) != 0u)
	{
		if (x == 0)
			left = 0.0;
		if (x + count >= pc.row_pixels)
			right += float(pc.pad);
	}
	gl_Position = box_position(left, float(y * pc.scale), right, float((y + 1) * pc.scale));
}
