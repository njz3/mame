// license:BSD-3-Clause
// copyright-holders:njz3
// Gaelco 3D triangles: render_triangle's extent of this pixel's row, then
// render_noz_noperspective, render_normal or render_alphablend: plane
// equations of u/z, v/z and 1/z, 16-bit depth, texture mask and the
// COLOR3D bilinear filter; the 50% blend as colour + 0.5 * destination
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_TEXTURE) readonly buffer texture_t { uint texram[]; };

layout(location = 0) flat in uint record;
layout(location = 0, index = 0) out vec4 out_colour;
layout(location = 0, index = 1) out vec4 out_factor;

float F(uint offset) { return uintBitsToFloat(data[record + offset]); }

// poly_manager::round_coordinate
int round_coordinate(float value)
{
	float ipart = floor(value);
	return int(ipart) + ((value - ipart > 0.5) ? 1 : 0);
}

uint texel(uint offset)
{
	return (texram[offset >> 2] >> ((offset & 3u) * 8u)) & 0xffu;
}

bool masked(uint offset)
{
	return (offset < pc.mask_size) && (((texram[pc.mask_offset + (offset >> 5)] >> (offset & 31u)) & 1u) != 0u);
}

uint palette(uint index)
{
	return data[pc.values + data[record + GR_PALETTE] + (index & 0x7fffu)];
}

// COLOR3D (Actel A1280XL) bilinear filter: 4-bit fractions, 8-bit output
uint color3d_filter(uint rgb00, uint rgb01, uint rgb10, uint rgb11, uint u, uint v)
{
	uint uf = (u & 0xffu) >> 4;
	uint vf = (v & 0xffu) >> 4;
	uvec4 weights = uvec4((16u - uf) * (16u - vf), uf * (16u - vf), (16u - uf) * vf, uf * vf);
	uvec4 texels = uvec4(rgb00, rgb01, rgb10, rgb11);
	uint r = 0u;
	uint g = 0u;
	uint b = 0u;
	for (int i = 0; i < 4; i++)
	{
		r += ((texels[i] >> 16) & 31u) * weights[i];
		g += ((texels[i] >> 11) & 31u) * weights[i];
		b += (texels[i] & 31u) * weights[i];
	}
	return 0xff000000u | ((r >> 5) << 16) | ((g >> 5) << 8) | (b >> 5);
}

vec4 colour_value(uint colour)
{
	return vec4(float((colour >> 16) & 0xffu), float((colour >> 8) & 0xffu), float(colour & 0xffu), float(colour >> 24)) * (1.0 / 255.0);
}

void main()
{
	int x = int(gl_FragCoord.x);
	int y = pc.height - 1 - int(gl_FragCoord.y);

	// the extent of this row
	precise float fully = float(y) + 0.5;
	precise float startx = F(GR_V1X) + (fully - F(GR_V1Y)) * F(GR_DXDY13);
	precise float stopx;
	if (fully < F(GR_V2Y))
		stopx = F(GR_V1X) + (fully - F(GR_V1Y)) * F(GR_DXDY12);
	else
		stopx = F(GR_V2X) + (fully - F(GR_V2Y)) * F(GR_DXDY23);
	int istartx = round_coordinate(startx);
	int istopx = round_coordinate(stopx);
	if (istartx > istopx)
	{
		int swap = istartx;
		istartx = istopx;
		istopx = swap;
	}
	istartx = max(istartx, int(data[record + GR_CLIPLEFT]));
	istopx = min(istopx, int(data[record + GR_CLIPRIGHT]));
	if ((x < istartx) || (x >= istopx))
		discard;

	// the plane equations at the first pixel of the extent, then this pixel
	uint mode = data[record + GR_FLAGS];
	float scanline = float(y);
	float first = float(istartx);
	float steps = float(x - istartx);
	int u, v;
	if (mode == GM_NOZ)
	{
		precise float zbase = 1.0 / F(GR_OOZ_BASE);
		precise float uoz_step = F(GR_UOZ_DX) * zbase;
		precise float voz_step = F(GR_VOZ_DX) * zbase;
		precise float uoz = (F(GR_UOZ_BASE) + scanline * F(GR_UOZ_DY) + first * F(GR_UOZ_DX)) * zbase + steps * uoz_step;
		precise float voz = (F(GR_VOZ_BASE) + scanline * F(GR_VOZ_DY) + first * F(GR_VOZ_DX)) * zbase + steps * voz_step;
		int zbufval = int(-F(GR_Z0) * zbase);
		u = int(uoz);
		v = int(voz);
		gl_FragDepth = float(uint(zbufval) & 0xffffu) * (1.0 / 65536.0);
	}
	else
	{
		precise float ooz = F(GR_OOZ_BASE) + scanline * F(GR_OOZ_DY) + first * F(GR_OOZ_DX) + steps * F(GR_OOZ_DX);
		if (!(ooz > 0.0))
			discard;
		precise float z = 1.0 / ooz;
		precise float uoz = F(GR_UOZ_BASE) + scanline * F(GR_UOZ_DY) + first * F(GR_UOZ_DX) + steps * F(GR_UOZ_DX);
		precise float voz = F(GR_VOZ_BASE) + scanline * F(GR_VOZ_DY) + first * F(GR_VOZ_DX) + steps * F(GR_VOZ_DX);
		int zbufval = int(F(GR_Z0) * z);
		u = int(uoz * z);
		v = int(voz * z);

		// the comparison uses the signed value, the buffer gets its magnitude
		if (zbufval >= 0)
			gl_FragDepth = float(min(zbufval, 0x10000)) * (1.0 / 65536.0);
		else
			gl_FragDepth = float(uint(-zbufval) & 0xffffu) * (1.0 / 65536.0);
	}

	uint endmask = pc.texture_size - 1u;
	uint pixeloffs = (data[record + GR_TEX] + uint((v >> 8) * 4096) + uint(u >> 8)) & endmask;
	if (masked(pixeloffs))
		discard;
	uint color = data[record + GR_COLOR];
	uint filtered = color3d_filter(
			palette(color + texel(pixeloffs)), palette(color + texel((pixeloffs + 1u) & endmask)),
			palette(color + texel((pixeloffs + 4096u) & endmask)), palette(color + texel((pixeloffs + 4097u) & endmask)),
			uint(u), uint(v));
	if (mode == GM_ALPHA)
	{
		out_colour = colour_value(0xff000000u | ((filtered & 0x00fefefeu) >> 1));
		out_factor = vec4(0.5);
	}
	else
	{
		out_colour = colour_value(filtered);
		out_factor = vec4(0.0);
	}
}
