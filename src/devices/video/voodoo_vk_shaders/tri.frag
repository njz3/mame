// license:BSD-3-Clause
// copyright-holders:njz3
// Voodoo pixel pipeline, ported from voodoo_render.cpp: the iterated values,
// texture fetches, texture and colour combine units, chroma key, alpha
// tests and fog run here with the same integer arithmetic; the depth test,
// alpha blending and writes are done by the fixed-function stages
#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_gpu_shader_int64 : require

#include "layout.h"

layout(std430, set = 0, binding = VKB_RECORDS) readonly buffer records_t { uint rec[]; };
layout(std430, set = 0, binding = VKB_LOOKUP) readonly buffer lookup_t { uint lut[]; };
layout(std430, set = 0, binding = VKB_TMU0) readonly buffer tmu0_t { uint ram0[]; };
layout(std430, set = 0, binding = VKB_TMU1) readonly buffer tmu1_t { uint ram1[]; };

layout(location = 0) flat in uint base;
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_prefog;

const int dither4x4[16] = int[16](
	 0,  8,  2, 10,
	12,  4, 14,  6,
	 3, 11,  1,  9,
	15,  7, 13,  5);

const int log2_table[128] = int[128](
	  0,   2,   5,   8,  11,  14,  16,  19,  22,  25,  27,  30,  33,  35,  38,  40,
	 43,  46,  48,  51,  53,  56,  58,  61,  63,  65,  68,  70,  73,  75,  77,  80,
	 82,  84,  87,  89,  91,  93,  96,  98, 100, 102, 104, 106, 109, 111, 113, 115,
	117, 119, 121, 123, 125, 127, 129, 132, 134, 136, 138, 140, 141, 143, 145, 147,
	149, 151, 153, 155, 157, 159, 161, 162, 164, 166, 168, 170, 172, 173, 175, 177,
	179, 181, 182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201, 203, 205,
	206, 208, 209, 211, 213, 214, 216, 218, 219, 221, 222, 224, 225, 227, 229, 230,
	232, 233, 235, 236, 238, 239, 241, 242, 244, 245, 247, 248, 250, 251, 253, 254);


//-------------------------------------------------
//  record and memory access
//-------------------------------------------------

uint R(uint off) { return rec[base + off]; }
int RI(uint off) { return int(rec[base + off]); }
int64_t R64(uint off) { return int64_t(packUint2x32(uvec2(rec[base + off], rec[base + off + 1u]))); }
bool B(uint value, int bit) { return ((value >> uint(bit)) & 1u) != 0u; }

// texture iteration values: S, T, W start, X gradients, Y gradients
int64_t ST(uint blk, uint index) { return R64(blk + VKT_ST + 2u * index); }

uint ram_word(bool tmu1, uint index) { return tmu1 ? ram1[index] : ram0[index]; }

// colours are (r, g, b, a)
ivec4 argb(uint v) { return ivec4(int((v >> 16) & 0xffu), int((v >> 8) & 0xffu), int(v & 0xffu), int(v >> 24)); }

int to_int(float v) { return int(clamp(v, -2147483648.0, 2147483520.0)); }


//-------------------------------------------------
//  helpers from voodoo_render.cpp
//-------------------------------------------------

// log2 of a value as 24.8, from the top 7 bits of the mantissa
int fast_log2(float value, int fracbits)
{
	if (value < 0.0)
		return 0;
	uint b = floatBitsToUint(value);
	int e = int((b >> 23) & 0xffu) - 127 - fracbits;
	return (e << 8) | log2_table[(b >> 16) & 127u];
}

int clz64(uint64_t v)
{
	uint hi = uint(v >> 32);
	uint lo = uint(v);
	if (hi != 0u)
		return 31 - findMSB(hi);
	if (lo != 0u)
		return 63 - findMSB(lo);
	return 64;
}

int compute_wfloat(int64_t iterw)
{
	int e = clz64(uint64_t(iterw)) - 16;
	if (e < 0)
		return 0x0000;
	if (e >= 16)
		return 0xffff;
	return ((e << 12) | (int(iterw >> (35 - e)) ^ 0x1fff)) + 1;
}

int clamp_channel(int v)
{
	if (v == 0xfff)
		return 0;
	if (v == 0x100)
		return 0xff;
	return v & 0xff;
}

ivec4 clamped_argb(ivec4 iter, uint fbzcp)
{
	ivec4 result = ivec4(uvec4(iter) >> 20u);
	if (B(fbzcp, 28))
		return clamp(result, 0, 255);
	return ivec4(clamp_channel(result.r), clamp_channel(result.g), clamp_channel(result.b), clamp_channel(result.a));
}

int clamped_z(int iterz, uint fbzcp)
{
	if (B(fbzcp, 28))
		return clamp(iterz >> 12, 0, 0xffff);
	uint result = uint(iterz) >> 12;
	if (result == 0xfffffu)
		return 0;
	if (result == 0x10000u)
		return 0xffff;
	return int(result & 0xffffu);
}

int clamped_w(int64_t iterw, uint fbzcp)
{
	if (B(fbzcp, 28))
		return clamp(int(iterw >> 48), 0, 0xff);
	uint result = uint(uint64_t(iterw) >> 48);
	if (result == 0xffffu)
		return 0;
	if (result == 0x100u)
		return 0xff;
	return int(result & 0xffu);
}

int compute_depthval(uint fbzmode, uint fbzcp, int wfloat, int iterz)
{
	int result;
	if (B(fbzmode, 3))
	{
		if (!B(fbzmode, 21))
			result = wfloat;
		else if ((uint(iterz) & 0xf0000000u) != 0u)
			result = 0x0000;
		else if ((uint(iterz) & 0x0ffff000u) == 0u)
			result = 0xffff;
		else
		{
			int e = (31 - findMSB(uint(iterz))) - 4;
			return ((e << 12) | ((iterz >> (15 - e)) ^ 0x1fff)) + 1;
		}
	}
	else
		result = clamped_z(iterz, fbzcp);

	if (B(fbzmode, 16))
		result = clamp(result + (int(R(VKR_ZACOLOR) << 16) >> 16), 0, 0xffff);
	return result;
}


//-------------------------------------------------
//  textures
//-------------------------------------------------

ivec4 lookup_texel(uint blk, uint format, uint texbase, int s, int t, bool tmu1)
{
	uint mask = R(blk + VKT_RAMMASK);
	uint lookup = R(blk + VKT_LOOKUP);
	if (format < 8u)
	{
		uint addr = (texbase + uint(t + s)) & mask;
		uint value = (ram_word(tmu1, addr >> 2) >> ((addr & 3u) * 8u)) & 0xffu;
		return argb(lut[lookup + value]);
	}
	uint addr = (texbase + 2u * uint(t + s)) & mask;
	uint value = (ram_word(tmu1, addr >> 2) >> ((addr & 2u) * 8u)) & 0xffffu;
	if (format >= 10u && format <= 12u)
		return argb(lut[lookup + value]);
	return argb((lut[lookup + (value & 0xffu)] & 0xffffffu) | ((value & 0xff00u) << 16));
}

int compute_lodbase(uint blk)
{
	float fdsdx = float(ST(blk, 3u));
	float fdtdx = float(ST(blk, 4u));
	float fdsdy = float(ST(blk, 6u));
	float fdtdy = float(ST(blk, 7u));
	float texdx = fdsdx * fdsdx + fdtdx * fdtdx;
	float texdy = fdsdy * fdsdy + fdtdy * fdtdy;
	return fast_log2(max(texdx, texdy), 64) / 2;
}

ivec4 fetch_texel(uint blk, uint texmode, int x, int ditherrow, int dx, int dy, inout int lod, bool tmu1)
{
	int64_t iters = ST(blk, 0u) + int64_t(dy) * ST(blk, 6u) + int64_t(dx) * ST(blk, 3u);
	int64_t itert = ST(blk, 1u) + int64_t(dy) * ST(blk, 7u) + int64_t(dx) * ST(blk, 4u);
	int64_t iterw = ST(blk, 2u) + int64_t(dy) * ST(blk, 8u) + int64_t(dx) * ST(blk, 5u);

	// S/T in 24.8
	int s, t;
	if (B(texmode, 0))
	{
		float fw = float(iterw);
		float recip = 256.0 / fw;
		s = to_int(float(iters) * recip);
		t = to_int(float(itert) * recip);
		lod -= fast_log2(fw, 32);
	}
	else
	{
		s = to_int(float(iters) * (1.0 / 16777216.0));
		t = to_int(float(itert) * (1.0 / 16777216.0));
	}
	if (B(texmode, 3) && iterw < 0)
	{
		s = 0;
		t = 0;
	}

	// clamp the LOD after applying bias and dither
	int lodmin = RI(blk + VKT_LODMIN);
	lod += RI(blk + VKT_LODBIAS);
	if (B(texmode, 4))
		lod += dither4x4[ditherrow + (x & 3)] << 4;
	lod = min(max(lod, lodmin), RI(blk + VKT_LODMAX));

	// if we don't own this LOD, take the next one
	int lodmask = RI(blk + VKT_LODMASK);
	int ilod = lod >> 8;
	ilod += (~lodmask >> ilod) & 1;
	ilod = min(ilod, ((lodmask & 0x100) != 0) ? 8 : 7);

	uint texbase = R(blk + VKT_LODOFFSET + uint(ilod));
	int smax = RI(blk + VKT_WMASK) >> ilod;
	int tmax = RI(blk + VKT_HMASK) >> ilod;
	uint format = (texmode >> 8) & 15u;
	bool clamps = B(texmode, 6);
	bool clampt = B(texmode, 7);

	if ((lod == lodmin && !B(texmode, 2)) || (lod != lodmin && !B(texmode, 1)))
	{
		// point sampled
		s >>= ilod + 8;
		t >>= ilod + 8;
		if (clamps)
			s = clamp(s, 0, smax);
		if (clampt)
			t = clamp(t, 0, tmax);
		s &= smax;
		t &= tmax;
		t *= smax + 1;
		return lookup_texel(blk, format, texbase, s, t, tmu1);
	}

	// bilinear
	s >>= ilod;
	t >>= ilod;
	s -= 0x80;
	t -= 0x80;
	int bilinear_mask = int((R(VKR_MISC) >> 8) & 0xffu);
	int sfrac = s & bilinear_mask;
	int tfrac = t & bilinear_mask;
	s >>= 8;
	t >>= 8;
	int s1 = s + 1;
	int t1 = t + 1;
	if (clamps)
	{
		if (s < 0)
			s = s1 = 0;
		else if (s >= smax)
			s = s1 = smax;
	}
	s &= smax;
	s1 &= smax;
	if (clampt)
	{
		if (t < 0)
			t = t1 = 0;
		else if (t >= tmax)
			t = t1 = tmax;
	}
	t &= tmax;
	t1 &= tmax;
	t *= smax + 1;
	t1 *= smax + 1;
	ivec4 c00 = lookup_texel(blk, format, texbase, s, t, tmu1);
	ivec4 c01 = lookup_texel(blk, format, texbase, s1, t, tmu1);
	ivec4 c10 = lookup_texel(blk, format, texbase, s, t1, tmu1);
	ivec4 c11 = lookup_texel(blk, format, texbase, s1, t1, tmu1);
	ivec4 top = (c00 * (256 - sfrac) + c01 * sfrac) >> 8;
	ivec4 bottom = (c10 * (256 - sfrac) + c11 * sfrac) >> 8;
	return (top * (256 - tfrac) + bottom * tfrac) >> 8;
}

int detail_factor(uint blk, int lod)
{
	int detailbias = RI(blk + VKT_DETAILBIAS);
	if (detailbias <= lod)
		return 0;
	int tmp = (((detailbias - lod) << RI(blk + VKT_DETAILSCALE)) >> 8) & 0xff;
	return min(tmp, RI(blk + VKT_DETAILMAX));
}

ivec4 combine_texture(uint blk, uint texmode, ivec4 c_local, ivec4 c_other, int lod)
{
	ivec4 blend_color = c_other;
	if (B(texmode, 12))
		blend_color.rgb = ivec3(0);
	if (B(texmode, 21))
		blend_color.a = 0;
	if (B(texmode, 13))
		blend_color.rgb -= c_local.rgb;
	if (B(texmode, 22))
		blend_color.a -= c_local.a;

	ivec4 factor = ivec4(0);
	switch ((texmode >> 14) & 7u)
	{
		case 1u: factor.rgb = c_local.rgb; break;
		case 2u: factor.rgb = ivec3(c_other.a); break;
		case 3u: factor.rgb = ivec3(c_local.a); break;
		case 4u: factor.rgb = ivec3(detail_factor(blk, lod)); break;
		case 5u: factor.rgb = ivec3(lod & 0xff); break;
		default: break;
	}
	switch ((texmode >> 23) & 7u)
	{
		case 1u: factor.a = c_local.a; break;
		case 2u: factor.a = c_other.a; break;
		case 3u: factor.a = c_local.a; break;
		case 4u: factor.a = detail_factor(blk, lod); break;
		case 5u: factor.a = lod & 0xff; break;
		default: break;
	}
	if (!B(texmode, 17))
		factor.rgb ^= ivec3(0xff);
	if (!B(texmode, 26))
		factor.a ^= 0xff;
	factor += 1;

	ivec4 add = ivec4(0);
	switch ((texmode >> 18) & 3u)
	{
		case 1u: add.rgb = c_local.rgb; break;
		case 2u: add.rgb = ivec3(c_local.a); break;
		default: break;
	}
	if (((texmode >> 27) & 3u) != 0u)
		add.a = c_local.a;

	blend_color = clamp(((blend_color * factor) >> 8) + add, 0, 255);
	if (B(texmode, 20))
		blend_color.rgb ^= ivec3(0xff);
	if (B(texmode, 29))
		blend_color.a ^= 0xff;
	return blend_color;
}


//-------------------------------------------------
//  colour path
//-------------------------------------------------

bool chroma_key_test(ivec4 c)
{
	ivec4 key = argb(R(VKR_CHROMAKEY));
	uint range = R(VKR_CHROMARANGE);
	if (!B(range, 28))
		return !(c.r == key.r && c.g == key.g && c.b == key.b);

	int results = int(c.b >= key.b && c.b <= int(range & 0xffu));
	results ^= int((range >> 24) & 1u);
	results <<= 1;
	results |= int(c.g >= key.g && c.g <= int((range >> 8) & 0xffu));
	results ^= int((range >> 25) & 1u);
	results <<= 1;
	results |= int(c.r >= key.r && c.r <= int((range >> 16) & 0xffu));
	results ^= int((range >> 26) & 1u);
	if (B(range, 27))
		return results == 0;
	return results != 7;
}

bool combine_color(inout ivec4 color, ivec4 texel, int iterz, int64_t iterw, uint fbzcp, uint fbzmode)
{
	// c_other
	ivec4 c_other = ivec4(0);
	switch (fbzcp & 3u)
	{
		case 0u: c_other = color; break;
		case 1u: c_other = texel; break;
		case 2u: c_other = argb(R(VKR_COLOR1)); break;
		default: break;
	}
	if (B(fbzmode, 1) && !chroma_key_test(c_other))
		return false;

	// a_other
	switch ((fbzcp >> 2) & 3u)
	{
		case 0u: c_other.a = color.a; break;
		case 1u: c_other.a = texel.a; break;
		case 2u: c_other.a = int(R(VKR_COLOR1) >> 24); break;
		default: c_other.a = 0; break;
	}
	if (B(fbzmode, 13) && (c_other.a & 1) == 0)
		return false;

	// c_local and a_local
	ivec4 c_local;
	if (!B(fbzcp, 7))
		c_local = !B(fbzcp, 4) ? color : argb(R(VKR_COLOR0));
	else
		c_local = ((texel.a & 0x80) == 0) ? color : argb(R(VKR_COLOR0));
	switch ((fbzcp >> 5) & 3u)
	{
		case 1u: c_local.a = int(R(VKR_COLOR0) >> 24); break;
		case 2u: c_local.a = (clamped_z(iterz, fbzcp) >> 8) & 0xff; break;
		case 3u: c_local.a = clamped_w(iterw, fbzcp) & 0xff; break;
		default: c_local.a = color.a; break;
	}

	ivec4 blend_color = c_other;
	if (B(fbzcp, 8))
		blend_color.rgb = ivec3(0);
	if (B(fbzcp, 17))
		blend_color.a = 0;
	if (B(fbzcp, 9))
		blend_color.rgb -= c_local.rgb;
	if (B(fbzcp, 18))
		blend_color.a -= c_local.a;

	ivec4 factor = ivec4(0);
	switch ((fbzcp >> 10) & 7u)
	{
		case 1u: factor.rgb = c_local.rgb; break;
		case 2u: factor.rgb = ivec3(c_other.a); break;
		case 3u: factor.rgb = ivec3(c_local.a); break;
		case 4u: factor.rgb = ivec3(texel.a); break;
		case 5u: factor.rgb = texel.rgb; break;
		default: break;
	}
	switch ((fbzcp >> 19) & 7u)
	{
		case 1u: factor.a = c_local.a; break;
		case 2u: factor.a = c_other.a; break;
		case 3u: factor.a = c_local.a; break;
		case 4u: factor.a = texel.a; break;
		default: break;
	}
	if (!B(fbzcp, 13))
		factor.rgb ^= ivec3(0xff);
	if (!B(fbzcp, 22))
		factor.a ^= 0xff;

	ivec4 add = ivec4(0);
	switch ((fbzcp >> 14) & 3u)
	{
		case 1u: add.rgb = c_local.rgb; break;
		case 2u: add.rgb = ivec3(c_local.a); break;
		default: break;
	}
	if (((fbzcp >> 23) & 3u) != 0u)
		add.a = c_local.a;

	factor += 1;
	blend_color = clamp(((blend_color * factor) >> 8) + add, 0, 255);
	if (B(fbzcp, 25))
		blend_color.a ^= 0xff;
	if (B(fbzcp, 16))
		blend_color.rgb ^= ivec3(0xff);
	color = blend_color;
	return true;
}

bool alpha_test(uint alphamode, int alpha)
{
	int ref = RI(VKR_ALPHAREF);
	switch ((alphamode >> 1) & 7u)
	{
		case 0u: return false;
		case 1u: return alpha < ref;
		case 2u: return alpha == ref;
		case 3u: return alpha <= ref;
		case 4u: return alpha > ref;
		case 5u: return alpha != ref;
		case 6u: return alpha >= ref;
		default: return true;
	}
}

void apply_fogging(inout ivec4 color, uint fbzmode, uint fogmode, uint fbzcp, int x, int ditherrow, int wfloat, int iterz, int64_t iterw, int itera)
{
	ivec4 fog = argb(R(VKR_FOGCOLOR));
	if (B(fogmode, 5))
	{
		// constant fog, added to the colour when fog_mult is 0
		if (!B(fogmode, 2))
			fog = clamp(fog + color, 0, 255);
	}
	else
	{
		int fogblend = 0;
		if (B(fogmode, 1))
			fog = ivec4(0);
		if (!B(fogmode, 2))
			fog -= color;

		switch ((fogmode >> 3) & 3u)
		{
			case 0u:
			{
				int fog_depth = wfloat;
				if (B(fbzmode, 16))
					fog_depth = clamp(fog_depth + (int(R(VKR_ZACOLOR) << 16) >> 16), 0, 0xffff);
				uint entry = lut[R(VKR_FOGTABLE) + uint(fog_depth >> 10)];
				int delta = int(entry & 0xffu);
				int deltaval = (delta & int(R(VKR_MISC) & 0xffu)) * ((fog_depth >> 2) & 0xff);
				if (B(fogmode, 7) && (delta & 2) != 0)
					deltaval = -deltaval;
				deltaval >>= 6;
				if (B(fogmode, 6))
					deltaval += dither4x4[ditherrow + (x & 3)];
				deltaval >>= 4;
				fogblend = int((entry >> 8) & 0xffu) + deltaval;
				break;
			}
			case 1u: fogblend = itera; break;
			case 2u: fogblend = clamped_z(iterz, fbzcp) >> 8; break;
			case 3u: fogblend = clamped_w(iterw, fbzcp); break;
		}

		fogblend++;
		int scale = int(uint(fogblend) << 16) >> 16;
		fog = clamp((fog * scale) >> 8, 0, 255);
		if (!B(fogmode, 2))
			fog = clamp(fog + color, 0, 255);
	}
	color.rgb = fog.rgb;
}


//-------------------------------------------------
//  main
//-------------------------------------------------

void main()
{
	uint fbzcp = R(VKR_FBZCP);
	uint fbzmode = R(VKR_FBZMODE);
	uint alphamode = R(VKR_ALPHAMODE);
	uint fogmode = R(VKR_FOGMODE);

	// pixel position: the target rows are the frame buffer rows (scry)
	int x = int(gl_FragCoord.x);
	int scry = int(gl_FragCoord.y);
	int y = B(fbzmode, 17) ? RI(VKR_YORIGIN) - scry : scry;
	int dx = x - (RI(VKR_AX) >> 4);
	int dy = y - (RI(VKR_AY) >> 4);
	int ditherrow = (scry & 3) * 4;

	// stipple
	if (B(fbzmode, 2))
	{
		uint stipple = R(VKR_STIPPLE);
		if (B(fbzmode, 12))
		{
			int index = ((scry & 3) << 3) | (~x & 7);
			if (((stipple >> uint(index)) & 1u) == 0u)
				discard;
		}
		else
		{
			// rotate mode rotates once per pixel from the start of the
			// span; the screen X stands in for the span position
			uint r = uint(x + 1) & 31u;
			uint rotated = (r == 0u) ? stipple : ((stipple >> r) | (stipple << (32u - r)));
			if ((rotated & 0x80000000u) == 0u)
				discard;
		}
	}

	// iterated values
	ivec4 iterargb;
	iterargb.r = (RI(VKR_START + 0u) + dy * RI(VKR_DDY + 0u) + dx * RI(VKR_DDX + 0u)) << 8;
	iterargb.g = (RI(VKR_START + 1u) + dy * RI(VKR_DDY + 1u) + dx * RI(VKR_DDX + 1u)) << 8;
	iterargb.b = (RI(VKR_START + 2u) + dy * RI(VKR_DDY + 2u) + dx * RI(VKR_DDX + 2u)) << 8;
	iterargb.a = (RI(VKR_START + 3u) + dy * RI(VKR_DDY + 3u) + dx * RI(VKR_DDX + 3u)) << 8;
	int iterz = RI(VKR_START + 4u) + dy * RI(VKR_DDY + 4u) + dx * RI(VKR_DDX + 4u);
	int64_t iterw = (R64(VKR_W) + int64_t(dy) * R64(VKR_W + 4u) + int64_t(dx) * R64(VKR_W + 2u)) << 16;

	// depth: compared and written by the fixed-function stage
	int wfloat = compute_wfloat(iterw);
	int depthval = compute_depthval(fbzmode, fbzcp, wfloat, iterz);
	int depthsource = B(fbzmode, 20) ? int(R(VKR_ZACOLOR) & 0xffffu) : depthval;
	gl_FragDepth = float(depthsource) * (1.0 / 65536.0);

	// texture pipeline: TMU1 feeds TMU0
	ivec4 texel = ivec4(0);
	uint generic = R(VKR_GENERIC);
	if ((generic & 2u) != 0u)
	{
		uint texmode = R(VKR_TMU1 + VKT_TEXMODE);
		int lod = compute_lodbase(VKR_TMU1);
		ivec4 t1 = fetch_texel(VKR_TMU1, texmode, x, ditherrow, dx, dy, lod, true);
		texel = ((generic & 8u) != 0u) ? t1 : combine_texture(VKR_TMU1, texmode, t1, texel, lod);
	}
	if ((generic & 1u) != 0u)
	{
		uint texmode = R(VKR_TMU0 + VKT_TEXMODE);
		if (!B(texmode, 31))
		{
			int lod = compute_lodbase(VKR_TMU0);
			ivec4 t0 = fetch_texel(VKR_TMU0, texmode, x, ditherrow, dx, dy, lod, false);
			texel = ((generic & 4u) != 0u) ? t0 : combine_texture(VKR_TMU0, texmode, t0, texel, lod);
		}
		else
			texel = argb(R(VKR_TMUCONFIG));
	}

	// colour path
	ivec4 color = clamped_argb(iterargb, fbzcp);
	if (!combine_color(color, texel, iterz, iterw, fbzcp, fbzmode))
		discard;
	if (B(alphamode, 0) && !alpha_test(alphamode, color.a))
		discard;
	ivec4 prefog = color;
	if (B(fogmode, 0))
		apply_fogging(color, fbzmode, fogmode, fbzcp, x, ditherrow, wfloat, iterz, iterw, iterargb.a);

	out_color = vec4(color) * (1.0 / 255.0);
	out_prefog = vec4(prefog) * (1.0 / 255.0);
}
