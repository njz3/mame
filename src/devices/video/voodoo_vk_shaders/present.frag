// license:BSD-3-Clause
// copyright-holders:njz3
// a colour buffer through the CLUT of the DAC (256 red, green and blue values)
#version 450

layout(set = 0, binding = 0) uniform sampler2D source;
layout(std430, set = 0, binding = 1) readonly buffer clut_t { uint clut[]; };

layout(location = 0) out vec4 out_color;

void main()
{
	vec4 c = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
	uvec3 v = uvec3(c.rgb * 255.0 + 0.5);
	out_color = vec4(float(clut[v.r]), float(clut[256u + v.g]), float(clut[512u + v.b]), 255.0) * (1.0 / 255.0);
}
