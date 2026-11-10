// license:BSD-3-Clause
// copyright-holders:njz3
// the text layer over the polygons (mix_text_layer), then the C404 gamma
// tables; with -widescreen the text is in the middle or stretched
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(set = 0, binding = VB_SCENE) uniform sampler2D scene;

layout(location = 0) out vec4 out_color;

void main()
{
	ivec2 frag = ivec2(gl_FragCoord.xy);
	vec3 base = texelFetch(scene, frag, 0).rgb;
	int r = int(base.r * 255.0 + 0.5);
	int g = int(base.g * 255.0 + 0.5);
	int b = int(base.b * 255.0 + 0.5);

	int nx = (pc.stretch2d != 0) ? int(floor((float(frag.x) + 0.5) * 640.0 / float(pc.frame_width))) : ((frag.x - pc.pad) >= 0 ? (frag.x - pc.pad) / pc.scale : -1);
	int ny = frag.y / pc.scale;
	if ((nx >= 0) && (nx < 640) && (ny >= 0) && (ny < 480))
	{
		uint pen = data[pc.text + uint(ny) * 640u + uint(nx)];
		if (pen != 0xffffu)
		{
			uint rgb = data[pc.palette + pen];
			int tr = int((rgb >> 16) & 0xffu);
			int tg = int((rgb >> 8) & 0xffu);
			int tb = int(rgb & 0xffu);
			if (pc.text_fade != 0)
			{
				tr = ((tr * pc.fade_factor) + (int(pc.fade_rgb & 0xffu) * pc.fade_factor_inv)) >> 8;
				tg = ((tg * pc.fade_factor) + (int((pc.fade_rgb >> 8) & 0xffu) * pc.fade_factor_inv)) >> 8;
				tb = ((tb * pc.fade_factor) + (int((pc.fade_rgb >> 16) & 0xffu) * pc.fade_factor_inv)) >> 8;
			}
			if ((pc.alpha_factor_inv != 0) && (((pen & 0xfu) == pc.alpha_mask) || ((pen >= pc.alpha_low) && (pen <= pc.alpha_high))))
			{
				tr = ((tr * pc.alpha_factor) + (r * pc.alpha_factor_inv)) >> 8;
				tg = ((tg * pc.alpha_factor) + (g * pc.alpha_factor_inv)) >> 8;
				tb = ((tb * pc.alpha_factor) + (b * pc.alpha_factor_inv)) >> 8;
			}
			r = tr;
			g = tg;
			b = tb;
		}
	}

	r = int(data[pc.gamma + 0x100u + uint(r)] & 0xffu);
	g = int(data[pc.gamma + 0x200u + uint(g)] & 0xffu);
	b = int(data[pc.gamma + 0x300u + uint(b)] & 0xffu);
	out_color = vec4(float(r) / 255.0, float(g) / 255.0, float(b) / 255.0, 1.0);
}
