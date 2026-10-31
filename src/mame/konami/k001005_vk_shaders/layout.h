// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by k001005_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

// polygon record, what draw_scanline_generic reads from its object data
#define KR_TEXX         0       // texture x in texels
#define KR_TEXY         1
#define KR_TEXW         2       // texture width / 8 - 1
#define KR_TEXH         3
#define KR_PAGE         4       // texture page * 0x40000
#define KR_PAL          5       // texture palette * 256
#define KR_FLAGS        6       // KF_*
#define KR_DIFFUSE      7
#define KR_AMBIENT      8
#define KR_FOGCOLOR     9
#define KR_POLYCOLOR    10      // ARGB
#define KR_SIZE         11

#define KF_MIRROR       1
#define KF_FOG          2
#define KF_BILINEAR     4
#define KF_TEXTURE      8
#define KF_VERTEXCOLOR  16
#define KF_FBBLEND      32

// vertex: x, y in target pixels, z, fog, diffuse, u/w, v/w, 1/w, r, g, b,
// a, record word
#define KV_SIZE         16

// descriptor bindings
#define KB_TEXTURE      0       // texture ROM, 8-bit texels in pages of 0x40000
#define KB_FRAME        1       // palette (0x800 ARGB), records, vertices
