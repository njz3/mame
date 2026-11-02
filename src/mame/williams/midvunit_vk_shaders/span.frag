// license:BSD-3-Clause
// copyright-holders:njz3
// CPU writes into video RAM: the value of the native pixel
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat in uint values;
layout(location = 1) flat in int x0;
layout(location = 0) out uint out_index;

void main()
{
	int x = int(gl_FragCoord.x) / pc.scale;
	out_index = data[values + uint(x - x0)] & 0xffffu;
}
