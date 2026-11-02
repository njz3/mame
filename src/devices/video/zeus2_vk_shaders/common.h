// license:BSD-3-Clause
// copyright-holders:njz3
// declarations shared by the shaders

layout(std430, set = 0, binding = VB_DATA) readonly buffer data_t { uint data[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;      // of the frame buffer image
	uint base;          // first record
	uint states;        // state records
	uint values;        // span values, palettes
	int scale;
} pc;

// a frame buffer depth (24-bit integer) as a depth attachment value
float depth_value(int depth)
{
	return float(clamp(depth, 0, 1 << 24)) * (1.0 / 16777216.0);
}

// rgb_t as an attachment colour
vec4 colour_value(uint colour)
{
	return vec4(float((colour >> 16) & 0xffu), float((colour >> 8) & 0xffu), float(colour & 0xffu), float(colour >> 24)) * (1.0 / 255.0);
}

#ifdef VERTEX_SHADER
// box of target pixels, two triangles from the vertex index
vec4 box_position(float left, float top, float right, float bottom)
{
	uint corner = uint(gl_VertexIndex) % 6u;
	bool r = (corner == 1u) || (corner == 3u) || (corner == 4u);
	bool b = (corner == 2u) || (corner == 4u) || (corner == 5u);
	vec2 pos = vec2(r ? right : left, b ? bottom : top);
	return vec4(pos * pc.inv_size * 2.0 - 1.0, 0.0, 1.0);
}
#endif
