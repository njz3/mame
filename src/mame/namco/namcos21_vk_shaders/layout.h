// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by namcos21_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

// triangle record: the rows rendertri fills, each with its span of columns
// computed on the CPU exactly as the software rasterizer steps its edges
#define TR_TOP          0       // rows top .. bottom - 1
#define TR_BOTTOM       1
#define TR_LEFT         2       // columns of the drawn box
#define TR_RIGHT        3
#define TR_COLOR        4       // pen
#define TR_DEPTH        5       // zsort
#define TR_ROWS         6       // first row span (left | right << 16), from the values base
#define TR_SIZE         7

// mix modes
#define MIX_LAYER       0       // Winning Run: GPU 2D layer and 3D by priority
#define MIX_SPRITES     1       // Driver's Eyes: native sprites over and under the 3D

#define LAYER_BYTES     0x80000

// descriptor bindings
#define VB_LAYER        0       // Winning Run 2D layer, 8-bit pens
#define VB_DATA         1       // records and values
#define VB_PENS0        2       // the two frame buffers
#define VB_PENS1        3
