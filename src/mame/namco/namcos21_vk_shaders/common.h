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
	int mode;           // MIX_*
	int yscroll;        // Winning Run 2D layer
	int xscroll;
	uint layer_base;
	uint pri;
	uint fill;
	uint palette;       // first colour, from the values base
	uint colors;
	int pens;           // frame buffer shown
	int pens_width;     // frame buffer size
	int pens_height;
	int left;           // high-resolution screen pixel of target pixel (0, 0)
	int top;
	int native_left;    // native pixel of the first native value
	int native_top;
	int native_width;
	float narrow_middle; // Winning Run, anamorphic -widescreen: the 2D layer
	float narrow_factor; // narrowed (hires3d_output::narrow_params), 0: off
	int narrow_first;
	int narrow_last;
} pc;
