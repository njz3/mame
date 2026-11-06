// license:BSD-3-Clause
// copyright-holders:njz3
// TC0780FPA triangles and quads: the parameters at this pixel (a quad's
// along render_polygon's edges of this row), then the solid, shaded and
// textured scanline renderers with their 16-bit depth.  Stored depths are
// (depth + 1) / 65536: never below what the vertex shader gives the GPU, so
// that it can reject hidden pixels before running this shader.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_TEXTURE) readonly buffer texture_t { uint texram[]; };

layout(location = 0) flat in uint record;
layout(location = 1) flat in float solid;
layout(location = 2) noperspective in vec4 params;
layout(location = 0) out uint out_pen;
layout(depth_greater) out float gl_FragDepth;

float F(uint offset) { return uintBitsToFloat(data[record + offset]); }

// poly_manager::round_coordinate
int round_coordinate(float value)
{
	float ipart = floor(value);
	return int(ipart) + ((value - ipart > 0.5) ? 1 : 0);
}

void main()
{
	uint flags = data[record + PR_FLAGS];
	vec4 p;
	if (pc.kind == KIND_TRIANGLE)
	{
		p = params;
	}
	else
	{
		// render_polygon: the edges at this row, the parameters along them
		int x = int(gl_FragCoord.x);
		precise float fully = float(int(gl_FragCoord.y)) + 0.5;
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
		int istartx = round_coordinate(startx);
		int istopx = round_coordinate(stopx);
		if ((x < min(istartx, istopx)) || (x >= max(istartx, istopx)))
			discard;
		precise float oox = 1.0 / (stopx - startx);
		for (uint i = 0u; i < 4u; i++)
		{
			precise float lparam = F(l + QE_P + i) + ldy * F(l + QE_DPDY + i);
			precise float rparam = F(r + QE_P + i) + rdy * F(r + QE_DPDY + i);
			p[i] = lparam + (float(x) + 0.5 - startx) * ((rparam - lparam) * oox);
		}
	}

	int iz = int(p[0]) & 0xffff;
	gl_FragDepth = float(iz + 1) * (1.0 / 65536.0);

	uint mode = flags & PF_MODE;
	if (mode == PM_SOLID)
	{
		out_pen = uint(int(solid)) & 0xffffu;
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
