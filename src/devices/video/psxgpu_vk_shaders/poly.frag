// license:BSD-3-Clause
// copyright-holders:njz3
// a PlayStation pixel: texel from the native VRAM, 4/8/15 bits, texture
// window, interleaved textures, colour modulation; the blend is the pipeline's
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = 0) readonly buffer data_t { uint data[]; };
layout(push_constant) uniform constants_t { vec2 inv_size; float scale; int pass; uint records; } pc;

layout(location = 0) flat in uint record;
layout(location = 1) noperspective in vec3 colour;
layout(location = 2) noperspective in vec2 uv;
layout(location = 0) out vec4 out_color;

uint vram(int x, int y)
{
	uint offs = (uint(y & 1023) << 10) | uint(x & 1023);
	return (data[offs >> 1] >> ((offs & 1u) * 16u)) & 0xffffu;
}

void main()
{
	uint flags = data[record + uint(TR_FLAGS)];
	if ((flags & TF_TEXTURED) == 0u)
	{
		out_color = vec4(colour / 255.0, 1.0);
		return;
	}

	int u = int(floor(uv.x));
	int v = int(floor(uv.y));
	uint texel;
	if ((flags & TF_DIRECT) != 0u)
	{
		texel = vram(u, v);
	}
	else
	{
		uint tex = data[record + uint(TR_TEX)];
		uint clut = data[record + uint(TR_CLUT)];
		uint mode = data[record + uint(TR_MODE)];
		int tx = int(tex & 0xffffu);
		int ty = int(tex >> 16);
		int cx = int(clut & 0xffffu);
		int cy = int(clut >> 16);
		int tp = int(mode & 0xffu);
		u &= int((mode >> 8) & 0xffu);
		v &= int((mode >> 16) & 0xffu);
		if ((flags & TF_INTERLEAVED) != 0u)
		{
			if (tp == 0)
			{
				int xi = ((u >> 2) & ~0x3c) + ((v << 2) & 0x3c);
				int yi = (v & ~0xf) + ((u >> 4) & 0xf);
				texel = vram(cx + int((vram(tx + xi, ty + yi) >> ((u & 3) << 2)) & 0xfu), cy);
			}
			else if (tp == 1)
			{
				int xi = ((u >> 1) & ~0x78) + ((u << 2) & 0x40) + ((v << 3) & 0x38);
				int yi = (v & ~0x7) + ((u >> 5) & 0x7);
				texel = vram(cx + int((vram(tx + xi, ty + yi) >> ((u & 1) << 3)) & 0xffu), cy);
			}
			else
			{
				texel = vram(tx + u, ty + v);
			}
		}
		else if (tp == 0)
		{
			texel = vram(cx + int((vram(tx + (u >> 2), ty + v) >> ((u & 3) << 2)) & 0xfu), cy);
		}
		else if (tp == 1)
		{
			texel = vram(cx + int((vram(tx + (u >> 1), ty + v) >> ((u & 1) << 3)) & 0xffu), cy);
		}
		else
		{
			texel = vram(tx + u, ty + v);
		}

		// colour 0 is transparent; bit 15 chooses the semi-transparent texels
		if (texel == 0u)
			discard;
		bool semi = (texel & 0x8000u) != 0u;
		if ((pc.pass == PASS_OPAQUE) && semi)
			discard;
		if ((pc.pass == PASS_SEMI) && !semi)
			discard;
	}

	vec3 rgb = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u)) / 31.0;
	if ((flags & TF_RAW) == 0u)
		rgb = min(rgb * (colour / 128.0), vec3(1.0));
	out_color = vec4(rgb, 1.0);
}
