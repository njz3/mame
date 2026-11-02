// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by midvunit_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

// quad record: what render_polygon and the scanline renderers use
#define QR_FLAGS        0       // QF_* | left edges << 8 | right edges << 12
#define QR_PIXDATA      1
#define QR_TEXBASE      2       // byte offset in the texture RAM
#define QR_TOP          3       // rows top .. bottom - 1
#define QR_BOTTOM       4
#define QR_LEFT         5       // clip columns left .. right - 1
#define QR_RIGHT        6
#define QR_BOXLEFT      7       // columns covered by the drawn box
#define QR_BOXRIGHT     8
#define QR_MAXY         9       // float: Y of the bottom vertex
#define QR_EDGES        10      // three left edges, then three right edges
#define QR_SIZE         58

// an edge: floats
#define QE_X            0       // first vertex X, Y
#define QE_Y            1
#define QE_Y2           2       // second vertex Y
#define QE_DXDY         3
#define QE_P0           4       // parameters at the first vertex
#define QE_P1           5
#define QE_DP0DY        6
#define QE_DP1DY        7
#define QE_SIZE         8

#define QF_MODE         3u      // QM_*
#define QF_DITHER       4u
#define QF_INCLUSIVE    8u      // memset: the stop pixel is drawn too

#define QM_FLAT         0u
#define QM_TEX          1u
#define QM_TEXTRANS     2u
#define QM_TEXTRANSMASK 3u

// span record: CPU writes into a row, one word per pixel
#define SR_X            0       // native column, row
#define SR_Y            1
#define SR_COUNT        2
#define SR_VALUES       3       // first value, from the values base
#define SR_SIZE         4

#define TEXTURE_BYTES   0x400000

// descriptor bindings
#define VB_TEXTURE      0       // texture RAM, 8-bit texels
#define VB_DATA         1       // records, values, palettes
#define VB_PAGES        2       // the two video RAM pages
