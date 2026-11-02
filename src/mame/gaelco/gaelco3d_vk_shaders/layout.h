// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by gaelco3d_vk.cpp and its shaders, as 32-bit word offsets.

    A triangle record holds poly_manager::render_triangle's setup (rows,
    slopes) in high-resolution pixels and the polygon's plane equations;
    the fragment shader rebuilds the extent of its row and runs the scanline
    renderers.  Rows are logical (as the renderer numbers them); the image
    stores them upside down, as the screen shows them.

***************************************************************************/

#define GR_FLAGS        0       // GM_*
#define GR_TOP          1       // logical rows top .. bottom - 1
#define GR_BOTTOM       2
#define GR_BOXLEFT      3       // columns of the drawn box
#define GR_BOXRIGHT     4
#define GR_CLIPLEFT     5       // clip columns left .. right - 1
#define GR_CLIPRIGHT    6
#define GR_V1X          7       // floats: top and middle vertices
#define GR_V1Y          8
#define GR_V2X          9
#define GR_V2Y          10
#define GR_DXDY13       11      // floats: edge slopes
#define GR_DXDY12       12
#define GR_DXDY23       13
#define GR_TEX          14      // texture address
#define GR_COLOR        15      // palette base
#define GR_PALETTE      16      // palette snapshot, from the values base
#define GR_Z0           17      // floats: plane equations
#define GR_OOZ_BASE     18
#define GR_OOZ_DX       19
#define GR_OOZ_DY       20
#define GR_UOZ_BASE     21
#define GR_UOZ_DX       22
#define GR_UOZ_DY       23
#define GR_VOZ_BASE     24
#define GR_VOZ_DX       25
#define GR_VOZ_DY       26
#define GR_SIZE         27

// renderers
#define GM_NOZ          0u      // render_noz_noperspective
#define GM_NORMAL       1u      // render_normal
#define GM_ALPHA        2u      // render_alphablend

// descriptor bindings
#define VB_TEXTURE      0       // expanded texture bytes, then the mask bits
#define VB_DATA         1       // records and values
