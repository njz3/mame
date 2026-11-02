// license:BSD-3-Clause
// copyright-holders:njz3
// Zeus 2 triangles: render_triangle's extent of this pixel's row and the
// parameters at the pixel (integer depth steps, perspective-correct texture
// coordinates), then render_poly_8bit: depth floor, solid, RGB555, alpha
// and palette texels with the bilinear filter of rgbaint_t, and the pixel
// ALU blend as colour + factor * destination (dual-source blending)
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_WAVERAM) readonly buffer waveram_t { uint waveram[]; };

layout(location = 0) flat in uint record;
layout(location = 0, index = 0) out vec4 out_colour;
layout(location = 0, index = 1) out vec4 out_factor;

uint state;

float F(uint offset) { return uintBitsToFloat(data[record + offset]); }
uint S(uint offset) { return data[state + offset]; }

// poly_manager::round_coordinate
int round_coordinate(float value)
{
	float ipart = floor(value);
	return int(ipart) + ((value - ipart > 0.5) ? 1 : 0);
}

// wave RAM reads, wrapping within it like WAVERAM_WRAP
uint read8(uint byteoffs)
{
	byteoffs &= WAVERAM_BYTES - 1u;
	return (waveram[byteoffs >> 2] >> ((byteoffs & 3u) * 8u)) & 0xffu;
}

uint read16(uint wordoffs)
{
	uint byteoffs = (wordoffs * 2u) & (WAVERAM_BYTES - 1u);
	return (waveram[byteoffs >> 2] >> ((byteoffs & 2u) * 8u)) & 0xffffu;
}

// zeus2_device::get_texel_*
uint get_texel(uint format, uint base, int iy, int ix, uint width)
{
	uint y = uint(iy);
	uint x = uint(ix);
	if (format == TF_4BIT_2X2)
	{
		uint byteoffs = (y / 4u) * (width * 4u) + ((x / 4u) << 3) + ((y & 3u) << 1) + ((x / 2u) & 1u);
		return (read8(base + byteoffs) >> (4u * (x & 1u))) & 0x0fu;
	}
	if (format == TF_8BIT_4X2)
		return read8(base + (y / 2u) * (width * 2u) + ((x / 4u) << 3) + ((y & 1u) << 2) + (x & 3u));
	if (format == TF_8BIT_2X2_ALPHA)
		return read8(base + (((y / 2u) * (width * 2u) + ((x / 2u) << 2) + ((y & 1u) << 1) + (x & 1u)) << 1));
	return read8(base + (y / 4u) * (width * 4u) + ((x / 2u) << 3) + ((y & 3u) << 1) + (x & 1u));
}

// zeus2_device::get_alpha_8bit_2x2_alpha
uint get_alpha(uint base, int iy, int ix, uint width)
{
	uint y = uint(iy);
	uint x = uint(ix);
	return read8(base + (((y / 2u) * (width * 2u) + ((x / 2u) << 2) + ((y & 1u) << 1) + (x & 1u)) << 1) + 1u);
}

// zeus2_device::get_rgb555
uint get_rgb555(uint base, int iy, int ix, uint width)
{
	uint y = uint(iy);
	uint x = uint(ix);
	uint colour = read16(base / 2u + (y / 2u) * (width * 2u) + ((x / 2u) << 2) + ((y & 1u) << 1) + (x & 1u));
	return ((colour & 0x7c00u) << 9) | ((colour & 0x3e0u) << 6) | ((colour & 0x1fu) << 3);
}

uvec4 channels(uint colour)
{
	return uvec4((colour >> 16) & 0xffu, (colour >> 8) & 0xffu, colour & 0xffu, colour >> 24);
}

uint pack_channels(uvec4 c)
{
	return (c.a << 24) | (c.r << 16) | (c.g << 8) | c.b;
}

// rgbaint_t::bilinear_filter (SSE): each row interpolated, halved, then
// the rows interpolated
uint bilinear_filter(uint c00, uint c01, uint c10, uint c11, uint u, uint v)
{
	uvec4 row0 = channels(c00) * (256u - u) + channels(c01) * u;
	uvec4 row1 = channels(c10) * (256u - u) + channels(c11) * u;
	return pack_channels(((row0 >> 1u) * (256u - v) + (row1 >> 1u) * v) >> 15u);
}

// rgb_t::scale8
uint scale8(uint colour, uint scale)
{
	return pack_channels((channels(colour) * scale) >> 8u);
}

uint palette(uint index)
{
	return data[pc.values + S(PS_PALETTE) + index];
}

// zeus2_write_pixel: the colour, and the factor of the destination
void write_pixel(uint colour, uint flags)
{
	float factor = 0.0;
	if ((flags & PF_BLEND) != 0u)
	{
		uint src_alpha = S(PS_SRCALPHA);
		uint dst_alpha = S(PS_DSTALPHA);
		if (src_alpha == 0u)
			discard;
		if (src_alpha != 0x100u)
			colour = scale8(colour, src_alpha);
		factor = float(dst_alpha) * (1.0 / 256.0);
	}
	out_colour = colour_value(colour);
	out_factor = vec4(factor);
}

void main()
{
	int x = int(gl_FragCoord.x) - int(data[record + TR_WINX]);
	int y = int(gl_FragCoord.y) - int(data[record + TR_WINY]);
	state = pc.states + data[record + TR_STATE] * PS_SIZE;

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

	// the parameters at the first pixel of the extent, then this pixel's:
	// depth in integer steps, u/z, v/z and 1/z in float steps
	precise float fullstartx = float(istartx) + 0.5;
	int steps = x - istartx;
	precise float zstart = F(TR_START + 0u) + fullstartx * F(TR_DPDX + 0u) + fully * F(TR_DPDY + 0u);
	int curz = int(zstart) + steps * int(F(TR_DPDX + 0u));
	precise float curupz = F(TR_START + 1u) + fullstartx * F(TR_DPDX + 1u) + fully * F(TR_DPDY + 1u) + float(steps) * F(TR_DPDX + 1u);
	precise float curvpz = F(TR_START + 2u) + fullstartx * F(TR_DPDX + 2u) + fully * F(TR_DPDY + 2u) + float(steps) * F(TR_DPDX + 2u);
	precise float curooz = F(TR_START + 3u) + fullstartx * F(TR_DPDX + 3u) + fully * F(TR_DPDY + 3u) + float(steps) * F(TR_DPDX + 3u);

	uint flags = S(PS_FLAGS);
	int depth = ((flags & PF_DEPTH_CLEAR) != 0u) ? 0xffffff : max(curz, int(S(PS_ZMIN)));
	gl_FragDepth = depth_value(max(depth, 0));

	precise float ooz_inv = 1.0 / curooz;
	int curu = int(curupz * ooz_inv);
	int curv = int(curvpz * ooz_inv);
	int u0 = max(curu >> 8, 0);
	int v0 = max(curv >> 8, 0);
	int u1 = u0 + 1;
	int v1 = v0 + 1;
	uint ufrac = uint(curu) & 0xffu;
	uint vfrac = uint(curv) & 0xffu;

	uint mode = flags & PF_MODE;
	uint format = (flags & PF_FORMAT) >> PF_FORMAT_SHIFT;
	uint base = S(PS_TEXBASE);
	uint width = S(PS_TEXWIDTH);
	if (mode == PM_SOLID)
	{
		write_pixel(S(PS_SOLID), flags);
	}
	else if (mode == PM_RGB555)
	{
		out_colour = colour_value(get_rgb555(base, v0, u0, width));
		out_factor = vec4(0.0);
	}
	else if (mode == PM_ALPHA)
	{
		uint alpha0 = get_alpha(base, v0, u0, width);
		uint alpha1 = get_alpha(base, v0, u1, width);
		uint alpha2 = get_alpha(base, v1, u0, width);
		uint alpha3 = get_alpha(base, v1, u1, width);
		uint src_alpha = ((alpha0 * (256u - ufrac) + alpha1 * ufrac) * (256u - vfrac) + (alpha2 * (256u - ufrac) + alpha3 * ufrac) * vfrac) >> 16;
		if (src_alpha == 0u)
			discard;
		uint filtered = bilinear_filter(
				palette(get_texel(format, base, v0, u0, width)), palette(get_texel(format, base, v0, u1, width)),
				palette(get_texel(format, base, v1, u0, width)), palette(get_texel(format, base, v1, u1, width)),
				ufrac, vfrac);
		out_colour = colour_value(scale8(filtered, src_alpha));
		out_factor = vec4(float(0x100u - src_alpha) * (1.0 / 256.0));
	}
	else
	{
		uint transcolor = S(PS_TRANS);
		uint texel0 = get_texel(format, base, v0, u0, width);
		uint texel1 = get_texel(format, base, v0, u1, width);
		uint texel2 = get_texel(format, base, v1, u0, width);
		uint texel3 = get_texel(format, base, v1, u1, width);
		if ((texel0 == transcolor) || (texel1 == transcolor) || (texel2 == transcolor) || (texel3 == transcolor))
			discard;
		write_pixel(bilinear_filter(palette(texel0), palette(texel1), palette(texel2), palette(texel3), ufrac, vfrac), flags);
	}
}
