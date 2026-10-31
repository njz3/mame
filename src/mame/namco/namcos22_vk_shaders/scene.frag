// license:BSD-3-Clause
// copyright-holders:njz3
// System 22 polygons and sprites, ported from the scanline renderers of
// namcos22_v.cpp; the blend against the destination is done by the
// fixed-function stage, the priority output marks the covered pixels
#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.h"

layout(location = 0) noperspective in vec4 param;
layout(location = 1) flat in uint record;

layout(location = 0) out vec4 out_color;
layout(location = 1) out uint out_pri;

uint R(uint off) { return frame[record + off]; }

void main()
{
	uint kind = R(NR_KIND);
	uint flags = R(NR_FLAGS);
	int fogfactor = int(R(NR_FOGFACTOR));
	int fadefactor = int(R(NR_FADEFACTOR));
	uint pen = 0u;
	ivec3 rgb;

	if (kind == NRK_SPRITE)
	{
		int y = int(param.y - float(R(NR_FLIPY)));
		int x = int(param.x - float(R(NR_FLIPX)));
		uint index = uint(int(R(NR_SOURCE)) + y * int(R(NR_MODULO)) + x);
		pen = byte_at(0u, min(index, uint(spr.length()) * 4u - 1u), true);
		if (pen == 0xffu)
			discard;
		rgb = pen_rgb(R(NR_PENBASE) + pen);
		if (fogfactor != 0xff)
			rgb = blend(rgb, rgb_of(R(NR_FOGCOLOR)), fogfactor);
		if (fadefactor != 0xff)
			rgb = blend(rgb, rgb_of(R(NR_FADECOLOR)), fadefactor);
	}
	else
	{
		// the parameters are 1/z, u/z, v/z and i/z
		float ooz = 1.0 / param.x;
		if ((flags & NRF_TEXTURE) != 0u)
		{
			int tx = int(param.y * ooz) & 0xfff;
			int ty = (int(param.z * ooz) & 0xfff) | int(R(NR_BANK));
			uint to = uint(((ty << 4) & 0xfff00) | (tx >> 4));
			uint tile = (tex[NT_TILEMAP + (to >> 1)] >> ((to & 1u) * 16u)) & 0xffffu;
			uint attr = byte_at(NT_TILEATTR, to, false);
			uint pixel = byte_at(NT_AYX, (attr << 8) | (uint(ty << 4) & 0xf0u) | uint(tx & 0xf), false);
			pen = byte_at(NT_TILEDATA, (tile << 8) | pixel, false);
		}
		rgb = pen_rgb(R(NR_PENBASE) + ((pen >> R(NR_PENSHIFT)) & R(NR_PENMASK)));

		bool shade = (flags & NRF_SHADE) != 0u;
		int shadeval = int(param.w * ooz);
		if (kind == NRK_POLY)
		{
			// fog, then shading
			if (fogfactor != 0xff)
				rgb = blend(rgb, rgb_of(R(NR_FOGCOLOR)), fogfactor);
			if (shade)
				rgb = clamp((rgb * (shadeval << 2)) >> 8, 0, 255);
		}
		else
		{
			// shading, fog, polygon fade, screen fade
			if (shade)
				rgb = clamp((rgb * (shadeval << 2)) >> 8, 0, 255);
			if (fogfactor != 0xff)
				rgb = blend(rgb, rgb_of(R(NR_FOGCOLOR)), fogfactor);
			if ((flags & NRF_POLYFADE) != 0u)
				rgb = scale_clamp(rgb, rgb_of(R(NR_POLYCOLOR)));
			if (fadefactor != 0xff)
				rgb = blend(rgb, rgb_of(R(NR_FADECOLOR)), fadefactor);
		}
	}

	// alpha against the destination
	int alphafactor = int(R(NR_ALPHA));
	float alpha = 1.0;
	if (alphafactor != 0xff && ((flags & NRF_ALPHA_ALL) != 0u || pen == R(NR_ALPHAPEN)))
		alpha = float(alphafactor) * (1.0 / 256.0);

	out_color = vec4(vec3(rgb) * (1.0 / 255.0), alpha);
	out_pri = 0x80u | R(NR_PRIO);
}
