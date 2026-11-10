// license:BSD-3-Clause
// copyright-holders:njz3
// declarations shared by the shaders

layout(std430, set = 0, binding = VB_DATA) readonly buffer data_t { uint data[]; };

layout(push_constant) uniform constants_t
{
	vec2 inv_size;
	uint palette;           // word offsets in data
	uint stencil;
	uint text;
	uint gamma;
	int scale;
	int pad;
	int frame_width;
	int stretch2d;
	uint tileid_mask;
	int text_fade;
	int fade_factor;
	int fade_factor_inv;
	uint fade_rgb;
	int alpha_factor;
	int alpha_factor_inv;
	uint alpha_mask;
	uint alpha_low;
	uint alpha_high;
} pc;
