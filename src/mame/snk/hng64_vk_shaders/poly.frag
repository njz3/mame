// license:BSD-3-Clause
// copyright-holders:njz3
// HNG64 triangles: render_triangle's extent of this pixel's row and the
// parameters at the pixel, then render_flat_scanline or
// render_texture_scanline (perspective divide, texture scroll, small pages,
// 4 or 8 bpp texels, light and blend bits); the floating point depth is
// stored as Z / 128, which keeps its order and the clear value of 100
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_TEXTURE) readonly buffer texture_t { uint texrom[]; };

layout(location = 0) flat in uint record;
layout(location = 0) out uint out_pen;

float F(uint offset) { return uintBitsToFloat(data[record + offset]); }

// poly_manager::round_coordinate
int round_coordinate(float value)
{
	float ipart = floor(value);
	return int(ipart) + ((value - ipart > 0.5) ? 1 : 0);
}

// C fmod: the remainder with the sign of the dividend
float c_fmod(float x, float y)
{
	return x - y * trunc(x / y);
}

uint texel(uint offset)
{
	offset &= TEXTURE_BYTES - 1u;
	return (texrom[offset >> 2] >> ((offset & 3u) * 8u)) & 0xffu;
}

void main()
{
	int x = int(gl_FragCoord.x);
	int y = int(gl_FragCoord.y);

	// the extent of this row
	precise float fully = float(y) + 0.5;
	precise float startx = F(TR_V1X) + (fully - F(TR_V1Y)) * F(TR_DXDY13);
	precise float stopx;
	if (fully < F(TR_V2Y))
		stopx = F(TR_V1X) + (fully - F(TR_V1Y)) * F(TR_DXDY12);
	else
		stopx = F(TR_V2X) + (fully - F(TR_V2Y)) * F(TR_DXDY23);
	int istartx = round_coordinate(startx);
	int istopx = round_coordinate(stopx);
	if (istartx > istopx)
	{
		int swap = istartx;
		istartx = istopx;
		istopx = swap;
	}
	istartx = max(istartx, int(data[record + TR_CLIPLEFT]));
	istopx = min(istopx, int(data[record + TR_CLIPRIGHT]));
	if ((x < istartx) || (x >= istopx))
		discard;

	// the parameters at this pixel
	precise float fullstartx = float(istartx) + 0.5;
	float steps = float(x - istartx);
	float p[5];
	for (uint i = 0u; i < 5u; i++)
	{
		precise float v = F(TR_START + i) + fullstartx * F(TR_DPDX + i) + fully * F(TR_DPDY + i) + steps * F(TR_DPDX + i);
		p[i] = v;
	}
	float z = p[P_Z];
	gl_FragDepth = z * (1.0 / 128.0);

	uint flags = data[record + TR_FLAGS];
	if ((flags & TF_FLAT) != 0u)
	{
		out_pen = (data[record + TR_PALOFFSET] + data[record + TR_COLORINDEX]) & 0xffffu;
		return;
	}

	// render_texture_scanline
	precise float w = p[P_W];
	precise float s_correct = p[P_S] / w;
	precise float t_correct = p[P_T] / w;
	precise float r_correct = p[P_LIGHT] / w;
	uint scroll = data[record + TR_TEXSCROLL];
	precise float texture_s = s_correct * 1024.0 + float(((scroll >> 16) & 0x3fffu) >> 5);
	precise float texture_t = t_correct * 1024.0 + float((scroll & 0x3fffu) >> 5);
	uint page = data[record + TR_TEXPAGE];
	if ((((page & 0xc000u) >> 14) & 2u) != 0u)
	{
		uint texmask = data[record + TR_TEXMASK];
		texture_t = c_fmod(texture_t, float(texmask & 0xffffu)) + 8.0 * float((page & 0x3f80u) >> 7);
		texture_s = c_fmod(texture_s, float(texmask >> 16)) + 8.0 * float(page & 0x007fu);
	}
	int t = int(texture_t) & 1023;
	int s = int(texture_s) & 1023;
	uint base = data[record + TR_TEXINDEX] * 1024u * 1024u;
	uint entry;
	if ((flags & TF_4BPP) != 0u)
	{
		entry = texel(base + uint(s * 512 + (t >> 1)));
		entry = ((t & 1) != 0) ? ((entry >> 4) & 0x0fu) : (entry & 0x0fu);
	}
	else
	{
		entry = texel(base + uint(s * 1024 + t));
	}

	// pen 0 in textures is always transparent
	if (entry == 0u)
		discard;
	uint lightval = uint(int(r_correct / 16.0)) & 0xffu;
	uint color = ((data[record + TR_PALOFFSET] + entry) & 0x7ffu) | (lightval << 12);
	if ((flags & TF_BLEND) != 0u)
		color |= 0x800u;
	out_pen = color & 0xffffu;
}
