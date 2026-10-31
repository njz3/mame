// license:BSD-3-Clause
// copyright-holders:njz3
// a native 16-bit value: RGB 5-6-5 for the colour pipeline, depth for the
// depth pipeline
#version 450

layout(location = 0) flat in uint value;
layout(location = 0) out vec4 out_color;

void main()
{
	uint r = (value >> 11) & 31u;
	uint g = (value >> 5) & 63u;
	uint b = value & 31u;
	out_color = vec4(float((r << 3) | (r >> 2)), float((g << 2) | (g >> 4)), float((b << 3) | (b >> 2)), 255.0) * (1.0 / 255.0);
	gl_FragDepth = float(value & 0xffffu) * (1.0 / 65536.0);
}
