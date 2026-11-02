// license:BSD-3-Clause
// copyright-holders:njz3
// TC0780FPA triangles and quads: render_triangle's or render_polygon's
// extent of this pixel's row and the parameters at the pixel, then the
// solid, shaded and textured scanline renderers with their 16-bit depth
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_TEXTURE) readonly buffer texture_t { uint texram[]; };

layout(location = 0) flat in uint record;
layout(location = 0) out uint out_pen;

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
	uint flags = data[record + PR_FLAGS];
	precise float fully = float(y) + 0.5;
	int istartx, istopx;
	vec4 start, dpdx;

	if (pc.kind == KIND_TRIANGLE)
	{
		// render_triangle: the extent of the row, the parameter planes at its
		// first pixel
		precise float startx = F(TR_V1X) + (fully - F(TR_V1Y)) * F(TR_DXDY13);
		precise float stopx;
		if (fully < F(TR_V2Y))
			stopx = F(TR_V1X) + (fully - F(TR_V1Y)) * F(TR_DXDY12);
		else
			stopx = F(TR_V2X) + (fully - F(TR_V2Y)) * F(TR_DXDY23);
		istartx = round_coordinate(startx);
		istopx = round_coordinate(stopx);
		if (istartx > istopx)
		{
			int swap = istartx;
			istartx = istopx;
			istopx = swap;
		}
		istartx = max(istartx, int(data[record + PR_CLIPLEFT]));
		istopx = min(istopx, int(data[record + PR_CLIPRIGHT]));
		precise float fullstartx = float(istartx) + 0.5;
		for (uint i = 0u; i < 4u; i++)
		{
			precise float p = F(TR_START + i) + fullstartx * F(TR_DPDX + i) + fully * F(TR_DPDY + i);
			start[i] = p;
			dpdx[i] = F(TR_DPDX + i);
		}
	}
	else
	{
		// render_polygon: the edges at this row, the parameters along them
		float maxy = F(QR_MAXY);
		uint lcount = (flags >> PF_LCOUNT_SHIFT) & 15u;
		uint rcount = (flags >> PF_RCOUNT_SHIFT) & 15u;
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
		istartx = round_coordinate(startx);
		istopx = round_coordinate(stopx);
		if (istartx > istopx)
		{
			int swap = istartx;
			istartx = istopx;
			istopx = swap;
		}
		istartx = max(istartx, int(data[record + PR_CLIPLEFT]));
		istopx = min(istopx, int(data[record + PR_CLIPRIGHT]));
		precise float oox = 1.0 / (stopx - startx);
		for (uint i = 0u; i < 4u; i++)
		{
			precise float lparam = F(l + QE_P + i) + ldy * F(l + QE_DPDY + i);
			precise float rparam = F(r + QE_P + i) + rdy * F(r + QE_DPDY + i);
			precise float d = (rparam - lparam) * oox;
			precise float p = lparam + (float(istartx) + 0.5 - startx) * d;
			start[i] = p;
			dpdx[i] = d;
		}
	}
	if ((x < istartx) || (x >= istopx))
		discard;

	// the parameters at this pixel
	float steps = float(x - istartx);
	precise vec4 p = start + steps * dpdx;
	int iz = int(p[0]) & 0xffff;
	gl_FragDepth = float(iz) * (1.0 / 65536.0);

	uint mode = flags & PF_MODE;
	if (mode == PM_SOLID)
	{
		out_pen = uint(int(start[1])) & 0xffffu;
	}
	else if (mode == PM_SHADE)
	{
		out_pen = uint(int(p[1])) & 0xffffu;
	}
	else
	{
		// render_texture_scan
		int palette = (int(p[3]) & 0x7f) << 8;
		uint wrap_x = (flags >> PF_WRAPX_SHIFT) & 3u;
		uint wrap_y = (flags >> PF_WRAPY_SHIFT) & 3u;
		int base_x = int(data[record + PR_TEXBASE] & 0xffffu);
		int base_y = int(data[record + PR_TEXBASE] >> 16);
		int iu, iv;
		if (wrap_x == 0u)
			iu = (int(p[1]) >> 4) & 0x7ff;
		else
			iu = (base_x + ((int(p[1]) >> 4) & ((0x08 << wrap_x) - 1))) & 0x7ff;
		if (wrap_y == 0u)
			iv = (int(p[2]) >> 4) & 0x7ff;
		else
			iv = (base_y + ((int(p[2]) >> 4) & ((0x08 << wrap_y) - 1))) & 0x7ff;
		uint offset = uint((iv << 11) + iu);
		uint texel = (texram[offset >> 2] >> ((offset & 3u) * 8u)) & 0xffu;
		if (texel == 0u)
			discard;
		out_pen = uint(palette) | texel;
	}
}
