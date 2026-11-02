// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by hng64_vk.cpp and its shaders, as 32-bit word offsets.

    A triangle record holds poly_manager::render_triangle's setup in
    high-resolution pixels (parameter planes of Z, W, light, S and T) and
    what the scanline renderers read from hng64_poly_data.

***************************************************************************/

#define TR_FLAGS        0       // TF_*
#define TR_TOP          1       // rows top .. bottom - 1
#define TR_BOTTOM       2
#define TR_BOXLEFT      3       // columns of the drawn box
#define TR_BOXRIGHT     4
#define TR_CLIPLEFT     5       // clip columns left .. right - 1
#define TR_CLIPRIGHT    6
#define TR_V1X          7       // floats: top and middle vertices
#define TR_V1Y          8
#define TR_V2X          9
#define TR_V2Y          10
#define TR_DXDY13       11      // floats: edge slopes
#define TR_DXDY12       12
#define TR_DXDY23       13
#define TR_START        14      // floats: 5 parameter starts, then dp/dx, then dp/dy
#define TR_DPDX         19
#define TR_DPDY         24
#define TR_TEXINDEX     29
#define TR_PALOFFSET    30
#define TR_COLORINDEX   31
#define TR_TEXPAGE      32      // texPageSmall
#define TR_TEXSCROLL    33      // texscrollx | texscrolly << 16
#define TR_TEXMASK      34      // tex_mask_x | tex_mask_y << 16
#define TR_SIZE         35

#define TF_FLAT         1u
#define TF_4BPP         2u
#define TF_BLEND        4u

// parameters
#define P_Z             0u
#define P_W             1u
#define P_LIGHT         2u
#define P_S             3u
#define P_T             4u

#define TEXTURE_BYTES   0x1000000

// descriptor bindings
#define VB_TEXTURE      0       // texture ROM, 8-bit texels
#define VB_DATA         1       // records and values
#define VB_PENS0        2       // the 3D colour buffer
#define VB_PENS1        3
