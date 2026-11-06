// license:BSD-3-Clause
// copyright-holders:njz3
// System 21 quads, as two triangles.  rendertri fills a row from the edges at
// its top, columns floor(left) .. floor(right) - 1: half a pixel down and to
// the left, the GPU's pixel centres cover the same pixels.
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint pen;

void main()
{
	const uint corners[6] = uint[6](0u, 1u, 2u, 2u, 3u, 0u);
	uint index = uint(gl_VertexIndex);
	uint record = pc.base + (index / 6u) * uint(TR_SIZE);
	uint corner = record + uint(TR_CORNERS) + corners[index % 6u] * 2u;
	vec2 pos = vec2(uintBitsToFloat(data[corner]) - 0.5, uintBitsToFloat(data[corner + 1u]) + 0.5);
	pen = data[record + uint(TR_COLOR)];
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, uintBitsToFloat(data[record + uint(TR_DEPTH)]), 1.0);
}
