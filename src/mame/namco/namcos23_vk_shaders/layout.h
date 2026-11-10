// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by namcos23_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

#define TR_FLAGS        0       // TF_*
#define TR_TBASE        1
#define TR_CMODE        2
#define TR_PALETTE      3
#define TR_IBASE        4       // float
#define TR_FADE         5       // factor | inverse << 16
#define TR_FADE_RGB     6       // r | g << 8 | b << 16
#define TR_POLY_RGB     7
#define TR_ALPHA        8       // alpha | pen << 16
#define TR_CLIP_LT      9       // left | top << 16, native
#define TR_CLIP_RB      10      // right | bottom << 16, exclusive
#define TR_VERTS        11
#define TV_X            0       // high-resolution pixels
#define TV_Y            1
#define TV_P            2       // z, u, v, i
#define TV_SIZE         6
#define TR_SIZE         (TR_VERTS + 3 * TV_SIZE)

#define TF_STENCIL      1u
#define TF_SHADE        2u
#define TF_POLY_FADE    4u
#define TF_COLOR_FADE   8u
#define TF_BLEND        16u
#define TF_POLY_ALPHA   32u
#define TF_ALPHA_ON     64u

// descriptor bindings
#define VB_TEXELS       0
#define VB_TILES        1
#define VB_DATA         2
#define VB_SCENE        3
