// license:BSD-3-Clause
// copyright-holders:njz3
// mame3d: texture with zero alpha as the transparent key (2D layers)
#version 450

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec4 v_tex;
layout(location = 0) out vec4 o_color;

void main()
{
	vec4 c = texture(tex, v_tex.xy);
	if (c.a == 0.0)
		discard;
	o_color = vec4(c.rgb * v_color.rgb, 1.0);
}
