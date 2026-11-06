// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by hng64_vk.cpp and its shaders, as 32-bit word offsets.

    A triangle record holds what the scanline renderers read from
    hng64_poly_data and its three vertices in high-resolution pixels, for
    the GPU to rasterise and interpolate.

***************************************************************************/

#define TR_FLAGS        0       // TF_*
#define TR_TEXINDEX     1
#define TR_PALOFFSET    2
#define TR_COLORINDEX   3
#define TR_TEXPAGE      4       // texPageSmall
#define TR_TEXSCROLL    5       // texscrollx | texscrolly << 16
#define TR_TEXMASK      6       // tex_mask_x | tex_mask_y << 16
#define TR_VERTS        7
#define TV_X            0       // floats
#define TV_Y            1
#define TV_P            2       // Z, W, light, S, T
#define TV_SIZE         7
#define TR_SIZE         (TR_VERTS + 3 * TV_SIZE)

#define TF_FLAT         1u
#define TF_4BPP         2u
#define TF_BLEND        4u

#define TEXTURE_BYTES   0x1000000

// descriptor bindings
#define VB_TEXTURE      0       // texture ROM, 8-bit texels
#define VB_DATA         1       // records and values
#define VB_PENS0        2       // the 3D colour buffer
#define VB_PENS1        3
