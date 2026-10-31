// license:BSD-3-Clause
// copyright-holders:njz3
// mame3d: shared vertex shader.  Positions are in target pixels; w is the
// clip-space w, so 1/w-perspective-correct interpolation follows from it.
#version 450

layout(push_constant) uniform push_block
{
	vec2 inv_size;
} pc;

layout(location = 0) in vec4 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_tex;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec4 v_tex;

void main()
{
	vec2 ndc = in_pos.xy * pc.inv_size * 2.0 - 1.0;
	gl_Position = vec4(ndc * in_pos.w, in_pos.z * in_pos.w, in_pos.w);
	v_color = in_color;
	v_tex = in_tex;
}
