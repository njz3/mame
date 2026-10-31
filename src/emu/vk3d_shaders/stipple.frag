// license:BSD-3-Clause
// copyright-holders:njz3
// mame3d: colour drawn on every other cell of a checkerboard (mesh
// translucency); v_tex.z is the cell size in pixels
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tex;
layout(location = 0) out vec4 o_color;

void main()
{
	ivec2 cell = ivec2(gl_FragCoord.xy / max(v_tex.z, 1.0));
	if (((cell.x ^ cell.y) & 1) != 0)
		discard;
	o_color = v_color;
}
