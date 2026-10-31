// license:BSD-3-Clause
// copyright-holders:njz3
// mame3d: flat or Gouraud colour
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tex;
layout(location = 0) out vec4 o_color;

void main()
{
	o_color = v_color;
}
