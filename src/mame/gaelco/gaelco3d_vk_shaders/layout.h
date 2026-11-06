// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by gaelco3d_vk.cpp and its shaders, as 32-bit word offsets.

    A triangle record holds its vertices in high-resolution pixels, for the
    GPU to rasterise, and the polygon's plane equations for the fragment
    shader.  Rows are logical (as the renderer numbers them); the image
    stores them upside down, as the screen shows them.

***************************************************************************/

#define GR_FLAGS        0       // GM_*
#define GR_VERTS        1       // floats: X, Y of the three vertices
#define GR_TEX          7       // texture address
#define GR_COLOR        8       // palette base
#define GR_PALETTE      9       // palette snapshot, from the values base
#define GR_Z0           10      // floats: plane equations
#define GR_OOZ_BASE     11
#define GR_OOZ_DX       12
#define GR_OOZ_DY       13
#define GR_UOZ_BASE     14
#define GR_UOZ_DX       15
#define GR_UOZ_DY       16
#define GR_VOZ_BASE     17
#define GR_VOZ_DX       18
#define GR_VOZ_DY       19
#define GR_SIZE         20

// renderers
#define GM_NOZ          0u      // render_noz_noperspective
#define GM_NORMAL       1u      // render_normal
#define GM_ALPHA        2u      // render_alphablend

// descriptor bindings
#define VB_TEXTURE      0       // expanded texture bytes, then the mask bits
#define VB_DATA         1       // records and values
