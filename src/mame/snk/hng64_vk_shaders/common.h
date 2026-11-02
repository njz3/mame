// license:BSD-3-Clause
// copyright-holders:njz3
// declarations shared by the shaders

layout(std430, set = 0, binding = VB_DATA) readonly buffer data_t { uint data[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;      // of the target
	uint base;          // triangles: first record; mix: native pixels
	uint values;        // values base
	int scale;
	int kind;
	uint palette;       // mix: first 3D colour, from the values base
	uint colors;
	int pens;           // 3D colour buffer
	int pens_width;     // its size
	int pens_height;
	int left;           // high-resolution screen pixel of target pixel (0, 0)
	int top;
	int native_left;    // native pixel of the first native value
	int native_top;
	int native_width;
	int blit;           // the 3D is blitted
	int xscroll;        // high-resolution blit scroll
	int yinc;           // vertical stretch, 16.16
	uint palbase;       // 3D palette half
	int vis_top;        // high-resolution top of the visible area
	int hleft;          // high-resolution left of the update
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
