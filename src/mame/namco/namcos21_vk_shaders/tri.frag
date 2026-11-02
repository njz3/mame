// license:BSD-3-Clause
// copyright-holders:njz3
// System 21 triangles: renderscanline_flat, the pen of the quad where its
// depth is below the stored one
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat in uint record;
layout(location = 0) out uint out_pen;

void main()
{
	int x = int(gl_FragCoord.x);
	int y = int(gl_FragCoord.y);
	uint span = data[pc.values + data[record + TR_ROWS] + uint(y - int(data[record + TR_TOP]))];
	if ((x < int(span & 0xffffu)) || (x >= int(span >> 16)))
		discard;
	out_pen = data[record + TR_COLOR];
	gl_FragDepth = float(data[record + TR_DEPTH]) * (1.0 / 65536.0);
}
