// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by zeus2_vk.cpp and its shaders, as 32-bit word offsets.

    A triangle record holds poly_manager::render_triangle's setup in
    high-resolution pixels relative to the render window: the fragment
    shader rebuilds the extent of its row and the parameters at its pixel
    from it, then runs render_poly_8bit with the state record.

***************************************************************************/

// triangle record
#define TR_STATE        0       // state record index
#define TR_TOP          1       // window rows top .. bottom - 1
#define TR_BOTTOM       2
#define TR_BOXLEFT      3       // window columns of the drawn box
#define TR_BOXRIGHT     4
#define TR_CLIPLEFT     5       // clip columns left .. right - 1
#define TR_CLIPRIGHT    6
#define TR_WINX         7       // target pixel of the window origin
#define TR_WINY         8
#define TR_V1X          9       // floats: top and middle vertices
#define TR_V1Y          10
#define TR_V2X          11
#define TR_V2Y          12
#define TR_DXDY13       13      // floats: edge slopes
#define TR_DXDY12       14
#define TR_DXDY23       15
#define TR_START        16      // floats: 4 parameter starts, then dp/dx, then dp/dy
#define TR_DPDX         20
#define TR_DPDY         24
#define TR_SIZE         28

// state record: zeus2_poly_extra_data
#define PS_FLAGS        0       // PM_* | texel format << 2 | PF_*
#define PS_TEXBASE      1       // byte offset in wave RAM
#define PS_TEXWIDTH     2
#define PS_SOLID        3       // RGB32
#define PS_TRANS        4
#define PS_SRCALPHA     5
#define PS_DSTALPHA     6
#define PS_ZMIN         7
#define PS_PALETTE      8       // first palette entry, from the values base
#define PS_SIZE         9

#define PF_MODE         3u
#define PF_FORMAT_SHIFT 2u
#define PF_FORMAT       (7u << PF_FORMAT_SHIFT)
#define PF_BLEND        32u
#define PF_DEPTH_CLEAR  64u

#define PM_SOLID        0u
#define PM_RGB555       1u
#define PM_ALPHA        2u      // 8-bit texels with 8-bit alpha
#define PM_TEXTURE      3u      // palette texels

#define TF_4BIT_2X2     0u
#define TF_8BIT_4X2     1u
#define TF_8BIT_2X2     2u
#define TF_8BIT_2X2_ALPHA 3u

// span record: frame buffer writes into a row, a colour and a depth per pixel
#define SR_X            0       // native column, row
#define SR_Y            1
#define SR_COUNT        2
#define SR_VALUES       3       // first value pair, from the values base
#define SR_FLAGS        4       // SF_*
#define SR_SIZE         5

#define SF_CONSTANT     1u      // one value pair for every pixel

#define WAVERAM_BYTES   0x1000000

// descriptor bindings
#define VB_WAVERAM      0       // wave RAM: textures and palettes
#define VB_DATA         1       // records and values
