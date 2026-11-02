// license:BSD-3-Clause
// copyright-holders:njz3
// the page shown, through the palette (32768 ARGB colours)
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(set = 0, binding = VB_PAGES) uniform usampler2D pages[2];

layout(location = 0) out vec4 out_color;

void main()
{
	ivec2 xy = ivec2(gl_FragCoord.xy) + ivec2(pc.xoffs, 0);
	uint index = (pc.page == 0) ? texelFetch(pages[0], xy, 0).r : texelFetch(pages[1], xy, 0).r;
	uint c = data[pc.values + (index & 0x7fffu)];
	out_color = vec4(float((c >> 16) & 0xffu), float((c >> 8) & 0xffu), float(c & 0xffu), float(c >> 24)) * (1.0 / 255.0);
}
