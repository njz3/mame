// license:BSD-3-Clause
// copyright-holders:njz3
// declarations shared by the shaders

layout(std430, set = 0, binding = VB_DATA) readonly buffer data_t { uint data[]; };

layout(push_constant) uniform push_t
{
	vec2 inv_size;      // of the frame image
	uint base;          // first record
	uint values;        // values base
	int height;         // frame image rows
	uint texture_size;  // bytes, a power of two
	uint mask_size;     // mask bits
	uint mask_offset;   // first mask word in the texture buffer
} pc;
