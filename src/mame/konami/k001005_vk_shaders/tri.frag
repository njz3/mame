// license:BSD-3-Clause
// copyright-holders:njz3
// K001005 pixel pipeline, ported from draw_scanline_generic: texel fetch
// from the K001006 texture ROM and palette, lighting, fog and the blends
// against the frame buffer.  Those blends are linear in the frame buffer
// colour, so the shader outputs the colour as A + K * destination: A in the
// first output, 1 - K in the second, added by dual-source blending.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"

layout(std430, set = 0, binding = KB_TEXTURE) readonly buffer texture_t { uint tex[]; };
layout(std430, set = 0, binding = KB_FRAME) readonly buffer frame_t { uint frame[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;
	uint vertices;
	uint palette;
} pc;

layout(location = 0) noperspective in vec4 zfd;
layout(location = 1) noperspective in vec4 uvw;
layout(location = 2) noperspective in vec4 rgba;
layout(location = 3) flat in uint record;

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_keep;

uint R(uint off) { return frame[record + off]; }

// colours as (a, r, g, b) like rgbaint_t
ivec4 argb(uint v) { return ivec4(int(v >> 24), int((v >> 16) & 0xffu), int((v >> 8) & 0xffu), int(v & 0xffu)); }

int mirror(int i, int sizeindex, bool mirrored)
{
	int size = (sizeindex + 1) * 8;
	if (mirrored && (i % (size * 2)) >= size)
		return (size - 1) - (i % size);
	return i % size;
}

// k001006_device::fetch_texel: pen 0 is transparent
ivec4 fetch(int u, int v)
{
	uint index = R(KR_PAGE) + uint(((v & 0x1ff) << 9) | (u & 0x1ff));
	uint texel = (tex[index >> 2] >> ((index & 3u) * 8u)) & 0xffu;
	if (texel == 0u)
		return ivec4(0);
	return argb(frame[pc.palette + ((R(KR_PAL) + texel) & 0x7ffu)]);
}

// the colour so far is A + K * frame buffer
vec4 A;
float K;

// rgbaint_t::blend against the frame buffer and against a colour
void blend_fb(int factor)
{
	float f = float(factor) * (1.0 / 256.0);
	A *= f;
	K = K * f + (1.0 - f);
}

void blend_color(ivec4 other, int factor)
{
	float f = float(factor) * (1.0 / 256.0);
	A = A * f + vec4(other) * (1.0 - f);
	K *= f;
}

void main()
{
	uint flags = R(KR_FLAGS);
	bool texture = (flags & KF_TEXTURE) != 0u;
	bool vertexcolor = (flags & KF_VERTEXCOLOR) != 0u;
	bool fog = (flags & KF_FOG) != 0u;
	bool fbblend = (flags & KF_FBBLEND) != 0u;
	bool bilinear = (flags & KF_BILINEAR) != 0u;
	ivec4 polycolor = argb(R(KR_POLYCOLOR));
	int polyalpha = polycolor.x;

	ivec4 texel = ivec4(0);
	int texel_alpha = 0;
	if (texture)
	{
		float oow = 1.0 / uvw.z;
		int iu = int(uvw.x * oow);
		int iv = int(uvw.y * oow);
		int tx = int(R(KR_TEXX));
		int ty = int(R(KR_TEXY));
		int tw = int(R(KR_TEXW));
		int th = int(R(KR_TEXH));
		bool mirrored = (flags & KF_MIRROR) != 0u;
		if (!bilinear)
		{
			texel = fetch(tx + mirror((iu >> 4) & 0x7f, tw, mirrored), ty + mirror((iv >> 4) & 0x7f, th, mirrored));
			texel_alpha = texel.x;
		}
		else
		{
			// sub-texel bias to avoid seams
			iu -= 7;
			iv -= 7;
			int ufrac = (iu & 0xf) * 16;
			int vfrac = (iv & 0xf) * 16;
			int u0 = tx + mirror((iu >> 4) & 0x7f, tw, mirrored);
			int u1 = tx + mirror(((iu >> 4) + 1) & 0x7f, tw, mirrored);
			int v0 = ty + mirror((iv >> 4) & 0x7f, th, mirrored);
			int v1 = ty + mirror(((iv >> 4) + 1) & 0x7f, th, mirrored);
			ivec4 t00 = fetch(u0, v0);
			ivec4 t01 = fetch(u1, v0);
			ivec4 t10 = fetch(u0, v1);
			ivec4 t11 = fetch(u1, v1);
			ivec4 top = (t00 * (256 - ufrac) + t01 * ufrac) >> 8;
			ivec4 bottom = (t10 * (256 - ufrac) + t11 * ufrac) >> 8;
			texel = (top * (256 - vfrac) + bottom * vfrac) >> 8;
			texel_alpha = t00.x;
		}
	}

	// light: diffuse scaled by the interpolated intensity, plus ambient
	int idiff = clamp(int(zfd.z), 0, 255);
	int ifog = clamp(int(zfd.y), 0, 255);
	ivec4 light = clamp(((argb(R(KR_DIFFUSE)) | ivec4(0xff, 0, 0, 0)) * idiff >> 8) + (argb(R(KR_AMBIENT)) | ivec4(0xff, 0, 0, 0)), 0, 255);
	ivec4 fogcolor = argb(R(KR_FOGCOLOR)) | ivec4(0xff, 0, 0, 0);

	K = 0.0;
	if (vertexcolor)
	{
		ivec4 c = clamp(ivec4(int(rgba.w), int(rgba.x), int(rgba.y), int(rgba.z)), 0, 255);
		int ia = c.x;
		if (ia == 0)
			discard;
		if (texture && texel_alpha != 0)
			c = texel;
		A = vec4(clamp((c * light) >> 8, 0, 255));
		if (fog)
			blend_color(fogcolor, ifog);
		if (fbblend)
			blend_fb(ia);
	}
	else if (texture)
	{
		if (texel_alpha == 0)
			discard;
		A = vec4(clamp((texel * light) >> 8, 0, 255));
		if (bilinear && texel_alpha < 0xff)
			blend_fb(texel_alpha);
		if (fog)
			blend_color(fogcolor, ifog);
		if (fbblend)
			blend_fb(polyalpha);
	}
	else
	{
		A = vec4(clamp((polycolor * light) >> 8, 0, 255));
		if (fog)
			blend_color(fogcolor, ifog);
		if (fbblend)
			blend_fb(polyalpha);
	}

	// (a, r, g, b) to the attachment's (r, g, b, a)
	out_color = vec4(A.y, A.z, A.w, A.x) * (1.0 / 255.0);
	out_keep = vec4(1.0 - K);
}
