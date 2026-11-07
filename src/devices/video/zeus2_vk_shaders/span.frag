// license:BSD-3-Clause
// copyright-holders:njz3
// frame buffer writes: the colour and depth of the native pixel
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat in uint values;
layout(location = 1) flat in int x0;
layout(location = 2) flat in uint flags;
layout(location = 0, index = 0) out vec4 out_colour;
layout(location = 0, index = 1) out vec4 out_factor;

void main()
{
	uint i = ((flags & SF_CONSTANT) != 0u) ? 0u : uint((int(gl_FragCoord.x) - pc.pad) / pc.scale - x0);
	out_colour = colour_value(data[values + i * 2u]);
	out_factor = vec4(0.0);
	gl_FragDepth = depth_value(int(data[values + i * 2u + 1u]));
}
