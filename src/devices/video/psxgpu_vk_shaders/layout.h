// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by psxgpu_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

#define TR_FLAGS        0       // TF_*
#define TR_TEX          1       // tx | ty << 16
#define TR_CLUT         2       // x | y << 16
#define TR_MODE         3       // tp | tww << 8 | twh << 16
#define TR_VERTS        4
#define TV_X            0
#define TV_Y            1
#define TV_R            2
#define TV_G            3
#define TV_B            4
#define TV_U            5
#define TV_V            6
#define TV_SIZE         7
#define TR_SIZE         (TR_VERTS + 3 * TV_SIZE)

#define TF_TEXTURED     1u
#define TF_RAW          2u
#define TF_INTERLEAVED  4u
#define TF_DIRECT       8u      // the VRAM itself at u, v: bit 15 and 0 mean nothing

// which texels a draw takes
#define PASS_ALL        0
#define PASS_OPAQUE     1
#define PASS_SEMI       2
