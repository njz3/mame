// license:BSD-3-Clause
// copyright-holders:njz3
// TC0780FPA triangles and quads, as the primitives themselves: a triangle's
// parameters are interpolated by the GPU, a quad's by the fragment shader
#version 450
#extension GL_GOOGLE_include_directive : require

#include "layout.h"
#include "common.h"

layout(location = 0) flat out uint record;
layout(location = 1) flat out float solid;              // render_solid_scan's colour
layout(location = 2) noperspective out vec4 params;     // triangles

float F(uint offset) { return uintBitsToFloat(data[offset]); }

void main()
{
	uint index = uint(gl_VertexIndex);
	vec2 pos;
	float z;
	if (pc.kind == KIND_QUAD)
	{
		// a lower bound of the depth: the real one is computed per pixel
		const uint corners[6] = uint[6](0u, 1u, 2u, 2u, 3u, 0u);
		record = pc.base + (index / 6u) * uint(QR_SIZE);
		uint corner = record + uint(QR_CORNERS) + corners[index % 6u] * 2u;
		pos = vec2(F(corner), F(corner + 1u));
		z = F(record + uint(QR_ZMIN));
		solid = F(record + uint(QR_EDGES + QE_P + 1));
		params = vec4(0.0);
	}
	else
	{
		record = pc.base + (index / 3u) * uint(TR_SIZE);
		uint vert = record + uint(TR_VERTS) + (index % 3u) * uint(TV_SIZE);
		pos = vec2(F(vert + uint(TV_X)), F(vert + uint(TV_Y)));
		params = vec4(F(vert + uint(TV_P)), F(vert + uint(TV_P + 1)), F(vert + uint(TV_P + 2)), F(vert + uint(TV_P + 3)));
		z = params[0];
		solid = params[1];
	}
	gl_Position = vec4(pos * pc.inv_size * 2.0 - 1.0, z * (1.0 / 65536.0), 1.0);
}
