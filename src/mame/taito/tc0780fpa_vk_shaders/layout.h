// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by tc0780fpa_vk.cpp and its shaders, as 32-bit word offsets.

    Triangles hold their three vertices, for the GPU to rasterise and
    interpolate.  Quads hold their corners, drawn as two triangles, and
    render_polygon's edges (parameters interpolated along the left and right
    edges of each row).  All in high-resolution pixels.

***************************************************************************/

// common header
#define PR_FLAGS        0       // PM_* | wrap x << 4 | wrap y << 6
#define PR_TEXBASE      1       // tex_base_x | tex_base_y << 16

// triangle: three vertices, floats
#define TR_VERTS        2
#define TV_X            0
#define TV_Y            1
#define TV_P            2       // Z, then colour or U, V and palette
#define TV_SIZE         6
#define TR_SIZE         (TR_VERTS + 3 * TV_SIZE)

// quad: floats; counts in the flags
#define QR_CORNERS      2       // X, Y of the four corners
#define QR_ZMIN         10      // lowest Z of the corners
#define QR_MAXY         11      // Y of the bottom vertex
#define QR_EDGES        12      // three left edges, then three right edges
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
