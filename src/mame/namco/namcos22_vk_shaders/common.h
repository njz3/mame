// license:BSD-3-Clause
// copyright-holders:njz3
// declarations shared by the System 22 shaders (they enable
// GL_GOOGLE_include_directive before including this)

#include "layout.h"

layout(std430, set = 0, binding = NB_TEXTURE) readonly buffer texture_t { uint tex[]; };
layout(std430, set = 0, binding = NB_SPRITE) readonly buffer sprite_t { uint spr[]; };
layout(std430, set = 0, binding = NB_FRAME) readonly buffer frame_t { uint frame[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;      // 1 / target size
	int scale;
	int pass;           // NP_*
	uint vertices;      // word offsets of the sections of the frame data
	uint records;
	uint palette;
	uint text;          // native text layer, u16
	uint textpri;       // native priority map, u8
	uint params;        // NM_*
	uint gamma;         // 3 x 256 words
	uint spot;          // 0x800 u16
	int native_width;
} pc;

// bytes and words packed in uint arrays
uint byte_at(uint base, uint index, bool sprite)
{
	uint word = sprite ? spr[base + (index >> 2)] : tex[base + (index >> 2)];
	return (word >> ((index & 3u) * 8u)) & 0xffu;
}

uint frame_u16(uint base, uint index)
{
	return (frame[base + (index >> 1)] >> ((index & 1u) * 16u)) & 0xffffu;
}

uint frame_u8(uint base, uint index)
{
	return (frame[base + (index >> 2)] >> ((index & 3u) * 8u)) & 0xffu;
}

// colours are (r, g, b) in 0-255
ivec3 rgb_of(uint v) { return ivec3(int((v >> 16) & 0xffu), int((v >> 8) & 0xffu), int(v & 0xffu)); }
ivec3 pen_rgb(uint pen) { return rgb_of(frame[pc.palette + (pen & 0x7fffu)]); }

// rgbaint_t::blend: factor parts of a, the rest of b
ivec3 blend(ivec3 a, ivec3 b, int factor) { return (a * factor + b * (256 - factor)) >> 8; }
ivec3 scale_clamp(ivec3 a, ivec3 s) { return clamp((a * s) >> 8, 0, 255); }
