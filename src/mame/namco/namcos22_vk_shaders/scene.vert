// license:BSD-3-Clause
// copyright-holders:njz3
// System 22 polygons and sprites: vertices read from the frame data
#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.h"

layout(location = 0) noperspective out vec4 param;
layout(location = 1) flat out uint record;

void main()
{
	uint v = pc.vertices + uint(gl_VertexIndex) * NV_SIZE;
	vec2 pos = vec2(uintBitsToFloat(frame[v]), uintBitsToFloat(frame[v + 1u]));
	param = vec4(uintBitsToFloat(frame[v + 2u]), uintBitsToFloat(frame[v + 3u]), uintBitsToFloat(frame[v + 4u]), uintBitsToFloat(frame[v + 5u]));
	record = frame[v + 6u];
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
