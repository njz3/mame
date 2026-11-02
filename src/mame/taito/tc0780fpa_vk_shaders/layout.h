// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by tc0780fpa_vk.cpp and its shaders, as 32-bit word offsets.

    Triangles keep poly_manager::render_triangle's setup (parameter planes),
    quads render_polygon's edges (parameters interpolated along the left and
    right edges of each row), both in high-resolution pixels.

***************************************************************************/

// common header
#define PR_FLAGS        0       // PM_* | wrap x << 4 | wrap y << 6
#define PR_TEXBASE      1       // tex_base_x | tex_base_y << 16
#define PR_TOP          2       // rows top .. bottom - 1
#define PR_BOTTOM       3
#define PR_BOXLEFT      4       // columns of the drawn box
#define PR_BOXRIGHT     5
#define PR_CLIPLEFT     6       // clip columns left .. right - 1
#define PR_CLIPRIGHT    7

// triangle: floats
#define TR_V1X          8       // top and middle vertices
#define TR_V1Y          9
#define TR_V2X          10
#define TR_V2Y          11
#define TR_DXDY13       12      // edge slopes
#define TR_DXDY12       13
#define TR_DXDY23       14
#define TR_START        15      // 4 parameter starts, then dp/dx, then dp/dy
#define TR_DPDX         19
#define TR_DPDY         23
#define TR_SIZE         27

// quad: floats; counts in the flags
#define QR_MAXY         8       // Y of the bottom vertex
#define QR_EDGES        9       // three left edges, then three right edges
#define QE_X            0       // first vertex X, Y
#define QE_Y            1
#define QE_Y2           2       // second vertex Y
#define QE_DXDY         3
#define QE_P            4       // 4 parameters at the first vertex
#define QE_DPDY         8       // 4 parameter slopes
#define QE_SIZE         12
#define QR_SIZE         (QR_EDGES + 6 * QE_SIZE)

#define PF_MODE         3u
#define PF_WRAPX_SHIFT  4u
#define PF_WRAPY_SHIFT  6u
#define PF_LCOUNT_SHIFT 8u      // quads: left and right edge counts
#define PF_RCOUNT_SHIFT 12u

#define PM_SOLID        0u
#define PM_SHADE        1u
#define PM_TEXTURE      2u

// primitive kinds
#define KIND_TRIANGLE   0
#define KIND_QUAD       1

#define TEXTURE_BYTES   0x400000

// descriptor bindings
#define VB_TEXTURE      0       // texture RAM, 8-bit texels, 2048 x 2048
#define VB_DATA         1       // records and values
#define VB_PENS0        2       // the two frame buffers
#define VB_PENS1        3
