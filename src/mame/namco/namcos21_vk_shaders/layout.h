// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by namcos21_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

// quad record, drawn as two triangles
#define TR_COLOR        0       // pen
#define TR_DEPTH        1       // float: zsort as a depth attachment value
#define TR_CORNERS      2       // floats: X, Y of the four corners
#define TR_SIZE         10

// mix modes
#define MIX_LAYER       0       // Winning Run: GPU 2D layer and 3D by priority
#define MIX_SPRITES     1       // Driver's Eyes: native sprites over and under the 3D

#define LAYER_BYTES     0x80000

// descriptor bindings
#define VB_LAYER        0       // Winning Run 2D layer, 8-bit pens
#define VB_DATA         1       // records and values
#define VB_PENS0        2       // the two frame buffers
#define VB_PENS1        3
