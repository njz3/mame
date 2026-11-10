// license:BSD-3-Clause
// copyright-holders:njz3
// namcos23_renderer::render_scanline at this pixel; the blend with what is
// under it is left to the GPU, with the factor in alpha
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(std430, set = 0, binding = VB_TEXELS) readonly buffer texels_t { uint texels[]; };
layout(std430, set = 0, binding = VB_TILES) readonly buffer tiles_t { uint tiles[]; };

layout(location = 0) flat in uint record;
layout(location = 1) noperspective in vec4 params;
layout(location = 0) out vec4 out_color;

void main()
{
	uint flags = data[record + uint(TR_FLAGS)];

	// the viewport, in high-resolution pixels of the frame
	uint lt = data[record + uint(TR_CLIP_LT)];
	uint rb = data[record + uint(TR_CLIP_RB)];
	int clip_left = int(lt & 0xffffu) * pc.scale + pc.pad;
	int clip_top = int(lt >> 16) * pc.scale;
	int clip_right = int(rb & 0xffffu) * pc.scale + pc.pad;
	int clip_bottom = int(rb >> 16) * pc.scale;
	if ((lt & 0xffffu) == 0u)
		clip_left = 0;
	if ((rb & 0xffffu) >= 640u)
		clip_right = pc.frame_width;
	ivec2 frag = ivec2(gl_FragCoord.xy);
	if ((frag.x < clip_left) || (frag.x >= clip_right) || (frag.y < clip_top) || (frag.y >= clip_bottom))
		discard;

	float ooz = 1.0 / params.x;
	uint u = uint(int(params.y * ooz));
	uint v = uint(int(params.z * ooz));

	if ((flags & TF_STENCIL) != 0u)
	{
		uint bit = (u & 15u) ^ 15u;
		uint offs = ((v << 6) | (u >> 4)) & 0x1ffffu;
		uint word = (data[pc.stencil + (offs >> 1)] >> ((offs & 1u) * 16u)) & 0xffffu;
		if (((word >> bit) & 1u) == 0u)
			discard;
	}

	v += data[record + uint(TR_TBASE)];

	// texture_lookup
	uint tile = tiles[((u >> 4) & 0xffu) | ((v << 4) & pc.tileid_mask)];
	if ((tile & 1u) != 0u)
		v = ~v;
	if ((tile & 2u) != 0u)
		u = ~u;
	if ((tile & 4u) != 0u)
	{
		uint t = u;
		u = v;
		v = t;
	}
	uint offset = (tile & 0xffffff00u) | ((v << 4) & 0xf0u) | (u & 0x0fu);
	uint pen = (texels[offset >> 2] >> ((offset & 3u) * 8u)) & 0xffu;

	uint cmode = data[record + uint(TR_CMODE)];
	uint colour = data[record + uint(TR_PALETTE)];
	uint penmask = 0xffu;
	uint penshift = 0u;
	if ((cmode & 4u) != 0u)
	{
		colour += 0xecu + ((cmode & 8u) << 1);
		penmask = 0x03u;
		penshift = 2u * (~cmode & 3u);
	}
	else if ((cmode & 2u) != 0u)
	{
		colour += 0xe0u + ((cmode & 8u) << 1);
		penmask = 0x0fu;
		penshift = 4u * (~cmode & 1u);
	}
	uint rgb = data[pc.palette + colour + ((pen >> penshift) & penmask)];
	int r = int((rgb >> 16) & 0xffu);
	int g = int((rgb >> 8) & 0xffu);
	int b = int(rgb & 0xffu);

	if ((flags & TF_SHADE) != 0u)
	{
		// six fractional bits: 0x40 is unity
		int shade = clamp(int(uintBitsToFloat(data[record + uint(TR_IBASE)]) + params.w * ooz), 0, 255);
		r = min((r * shade) >> 6, 255);
		g = min((g * shade) >> 6, 255);
		b = min((b * shade) >> 6, 255);
	}

	if ((flags & TF_POLY_FADE) != 0u)
	{
		uint c = data[record + uint(TR_POLY_RGB)];
		r = (r * int(c & 0xffu)) >> 8;
		g = (g * int((c >> 8) & 0xffu)) >> 8;
		b = (b * int((c >> 16) & 0xffu)) >> 8;
	}

	if ((flags & TF_COLOR_FADE) != 0u)
	{
		uint fade = data[record + uint(TR_FADE)];
		uint c = data[record + uint(TR_FADE_RGB)];
		int factor = int(fade & 0xffffu);
		int inverse = int(fade >> 16);
		r = ((r * factor) + (int(c & 0xffu) * inverse)) >> 8;
		g = ((g * factor) + (int((c >> 8) & 0xffu) * inverse)) >> 8;
		b = ((b * factor) + (int((c >> 16) & 0xffu) * inverse)) >> 8;
	}

	float alpha = 1.0;
	uint poly_alpha = data[record + uint(TR_ALPHA)];
	if (((flags & TF_POLY_ALPHA) != 0u) && (((flags & TF_ALPHA_ON) != 0u) || (pen == (poly_alpha >> 16))))
		alpha = float(poly_alpha & 0xffffu) / 256.0;
	else if ((flags & TF_BLEND) != 0u)
		alpha = 0.5;

	out_color = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, alpha);
}
