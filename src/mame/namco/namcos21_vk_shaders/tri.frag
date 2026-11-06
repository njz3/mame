// license:BSD-3-Clause
// copyright-holders:njz3
// System 21 quads: the pen of the quad
#version 450

layout(location = 0) flat in uint pen;
layout(location = 0) out uint out_pen;

void main()
{
	out_pen = pen;
}
