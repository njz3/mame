// license:BSD-3-Clause
// copyright-holders:njz3
// System 22 text layer mixing, fades and gamma, ported from
// namcos22_mix_text_layer and namcos22s_mix_text_layer: the text layer and
// its priority map are native, the scene and the output are magnified
#version 450
#extension GL_GOOGLE_include_directive : require

#include "common.h"

layout(set = 1, binding = NB_COLOR) uniform sampler2D scene_color;
layout(set = 1, binding = NB_PRI) uniform usampler2D scene_pri;

layout(location = 0) out vec4 out_color;
layout(location = 1) out uint out_pri;

uint P(uint off) { return frame[pc.params + off]; }

// Super System 22: the text pixel over dest, false when it leaves dest alone
bool super_mix(uint text, uint prival, ivec3 dest, out ivec3 rgb)
{
	bool spot = P(NM_SPOT_ON) != 0u;
	uint pen;
	rgb = dest;
	if (spot)
	{
		// remap pen
		pen = frame_u16(pc.spot, ((text << 2) | P(NM_SPOT_PALBASE)) & 0x3ffu) & 0xffu;
		if (pen < 0x80u)
			rgb = pen_rgb(pen | P(NM_TEXT_PALBASE));
		else if (prival != 6u)
			return false;
	}
	else
	{
		rgb = pen_rgb(text);
		pen = text & 0xffu;
	}

	if (spot && pen >= 0x80u)
	{
		// spot pen becomes brightness factor
		int factor = (int(P(NM_SPOT_FACTOR)) * int(pen & 0x7fu)) >> 7;
		rgb = clamp((dest * (0x100 - factor)) >> 8, 0, 255);
	}
	else
	{
		if (P(NM_FADE_ON) != 0u)
			rgb = blend(rgb, ivec3(P(NM_FADE_R), P(NM_FADE_G), P(NM_FADE_B)), int(P(NM_FADE_FACTOR)));
		uint alpha = P(NM_ALPHA_FACTOR);
		if (alpha != 0u && ((pen & 0xfu) == P(NM_ALPHA_MASK) || (pen >= P(NM_ALPHA_CHECK12) && pen <= P(NM_ALPHA_CHECK13))))
			rgb = blend(rgb, dest, int(0xffu - alpha));
	}
	return true;
}

void main()
{
	ivec2 xy = ivec2(gl_FragCoord.xy);

	// the native text pixel: in the middle of a -widescreen frame (none on
	// the sides), or stretched over its whole width
	int nx = (pc.stretch2d != 0)
			? int(floor((float(xy.x) + 0.5) * float(pc.native_width) * pc.inv_size.x))
			: ((xy.x - pc.pad >= 0) ? (xy.x - pc.pad) / pc.scale : -1);
	ivec2 nxy = ivec2(nx, xy.y / pc.scale);
	bool outside = (nx < 0) || (nx >= pc.native_width);
	uint nindex = uint(nxy.y * pc.native_width + max(nx, 0));
	uint text = outside ? 0u : frame_u16(pc.text, nindex);
	uint textpri = outside ? 0u : frame_u8(pc.textpri, nindex);
	bool super = P(NM_SUPER) != 0u;
	out_pri = 0u;

	// Super, before the scene: the text over the background
	if (pc.pass == NP_TEXT_BEFORE)
	{
		ivec3 rgb;
		if (textpri != 4u || !super_mix(text, 4u, rgb_of(P(NM_BACKGROUND)), rgb))
			discard;
		out_color = vec4(vec3(rgb) * (1.0 / 255.0), 1.0);
		return;
	}

	// the priority the software renderer would have: the text marks, with
	// bit 0 from the last polygon or sprite drawn there
	ivec3 pixel = ivec3(texelFetch(scene_color, xy, 0).rgb * 255.0 + 0.5);
	uint polypri = texelFetch(scene_pri, xy, 0).r;
	uint pri = ((polypri & 0x80u) != 0u) ? ((textpri & ~1u) | (polypri & 3u)) : textpri;

	if (super)
	{
		ivec3 rgb;
		if (pri == 6u && super_mix(text, 6u, pixel, rgb))
			pixel = rgb;
	}
	else
	{
		if (pri == 2u)
		{
			// shadow pens scale the scene, the others replace it
			uint pen = text & 0xffu;
			if (P(NM_SHADOW_ON) != 0u && pen >= 0xfcu && pen <= 0xfeu)
				pixel = scale_clamp(pixel, rgb_of(P(NM_MIX0 + pen - 0xfcu)));
			else
				pixel = pen_rgb(text);
		}

		// global fade; a fade to white makes the blacks scale up
		if (P(NM_FADE_ON) != 0u)
		{
			ivec3 fade = ivec3(P(NM_FADE_R), P(NM_FADE_G), P(NM_FADE_B));
			if (any(greaterThan(fade, ivec3(0x100))))
			{
				if (pixel.r == 0 && fade.r > 0x100) pixel.r = 1;
				if (pixel.g == 0 && fade.g > 0x100) pixel.g = 1;
				if (pixel.b == 0 && fade.b > 0x100) pixel.b = 1;
			}
			pixel = scale_clamp(pixel, fade);
		}
	}

	// gamma
	pixel = ivec3(int(frame[pc.gamma + uint(pixel.r)]), int(frame[pc.gamma + 256u + uint(pixel.g)]), int(frame[pc.gamma + 512u + uint(pixel.b)]));
	out_color = vec4(vec3(pixel) * (1.0 / 255.0), 1.0);
}
