// license:BSD-3-Clause
// copyright-holders:njz3
// declarations shared by the shaders

layout(std430, set = 0, binding = VB_DATA) readonly buffer data_t { uint data[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;      // of the target
	uint base;          // first record
	uint values;        // span values, palette
	int scale;
	int page;           // page shown
	int xoffs;          // page column of the first display column
} pc;

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
