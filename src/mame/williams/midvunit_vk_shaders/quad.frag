// license:BSD-3-Clause
// copyright-holders:njz3
// V-Unit quads: render_polygon's scanline setup evaluated at this pixel's
// row (left and right edges, rounded extent, parameters at the first
// pixel), then the scanline renderers (flat, textured, transparent,
// transparent mask) with their integer texture stepping and checkerboard
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_TEXTURE) readonly buffer texture_t { uint texram[]; };

layout(location = 0) flat in uint record;
layout(location = 0) out uint out_index;

float F(uint offset) { return uintBitsToFloat(data[record + offset]); }

// poly_manager::round_coordinate
int round_coordinate(float value)
{
	float ipart = floor(value);
	return int(ipart) + ((value - ipart > 0.5) ? 1 : 0);
}

void main()
{
	int x = int(gl_FragCoord.x);
	int y = int(gl_FragCoord.y);
	uint flags = data[record + QR_FLAGS];
	if (((flags & QF_DITHER) != 0u) && (((x ^ y) & 1) != 0))
		discard;

	// the edges at this scanline
	precise float fully = float(y) + 0.5;
	float maxy = F(QR_MAXY);
	uint lcount = (flags >> 8) & 15u;
	uint rcount = (flags >> 12) & 15u;
	uint l = QR_EDGES;
	for (uint i = 1u; (i < lcount) && (fully > F(l + QE_Y2)) && (fully < maxy); i++)
		l += QE_SIZE;
	uint r = QR_EDGES + 3u * QE_SIZE;
	for (uint i = 1u; (i < rcount) && (fully > F(r + QE_Y2)) && (fully < maxy); i++)
		r += QE_SIZE;
	precise float ldy = fully - F(l + QE_Y);
	precise float rdy = fully - F(r + QE_Y);
	precise float startx = F(l + QE_X) + ldy * F(l + QE_DXDY);
	precise float stopx = F(r + QE_X) + rdy * F(r + QE_DXDY);

	// the extent, clipped before the parameters are computed
	int istartx = round_coordinate(startx);
	int istopx = round_coordinate(stopx);
	if (istartx > istopx)
	{
		int swap = istartx;
		istartx = istopx;
		istopx = swap;
	}
	istartx = max(istartx, int(data[record + QR_LEFT]));
	istopx = min(istopx, int(data[record + QR_RIGHT]));
	int first = istartx;
	if (istartx >= istopx)
		istartx = istopx = 0;
	int stop = istopx + (((flags & QF_INCLUSIVE) != 0u) ? 1 : 0);
	if ((x < istartx) || (x >= stop))
		discard;

	uint pixdata = data[record + QR_PIXDATA];
	uint mode = flags & QF_MODE;
	if (mode == QM_FLAT)
	{
		out_index = pixdata & 0xffffu;
		return;
	}

	// 16.16 texture coordinates: the start truncated, then integer steps
	precise float oox = 1.0 / (stopx - startx);
	precise float lu = F(l + QE_P0) + ldy * F(l + QE_DP0DY);
	precise float ru = F(r + QE_P0) + rdy * F(r + QE_DP0DY);
	precise float dudx = (ru - lu) * oox;
	precise float ustart = lu + (float(first) + 0.5 - startx) * dudx;
	precise float lv = F(l + QE_P1) + ldy * F(l + QE_DP1DY);
	precise float rv = F(r + QE_P1) + rdy * F(r + QE_DP1DY);
	precise float dvdx = (rv - lv) * oox;
	precise float vstart = lv + (float(first) + 0.5 - startx) * dvdx;
	int u = int(ustart) + (x - istartx) * int(dudx);
	int v = int(vstart) + (x - istartx) * int(dvdx);

	int index = int(data[record + QR_TEXBASE]) + ((v >> 8) & 0xff00) + (u >> 16);
	uint pix = 0u;
	if ((index >= 0) && (index < TEXTURE_BYTES))
		pix = (texram[uint(index) >> 2] >> ((uint(index) & 3u) * 8u)) & 0xffu;
	if (mode == QM_TEX)
		out_index = (pixdata + pix) & 0xffffu;
	else if (pix == 0u)
		discard;
	else if (mode == QM_TEXTRANS)
		out_index = (pixdata + pix) & 0xffffu;
	else
		out_index = pixdata & 0xffffu;
}
