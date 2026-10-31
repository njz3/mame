// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by voodoo_vk.cpp and its shaders, as 32-bit word offsets.

    A triangle record holds what the Voodoo rasterizer reads from its
    poly_data, already scaled to the high resolution: the fragment shader
    rebuilds the iterated values of each pixel from them exactly like the
    software rasterizer does.  64-bit values are stored low word first.

***************************************************************************/

// triangle record
#define VKR_VX0          0      // vertex positions in target pixels (6 floats)
#define VKR_AX           6      // vertex A, 12.4, high-resolution screen pixels
#define VKR_AY           7
#define VKR_YORIGIN      8      // high-resolution Y origin
#define VKR_FBZCP        9
#define VKR_FBZMODE      10
#define VKR_ALPHAMODE    11
#define VKR_FOGMODE      12
#define VKR_GENERIC      13     // rasterizer_params generic flags
#define VKR_COLOR0       14
#define VKR_COLOR1       15
#define VKR_CHROMAKEY    16
#define VKR_CHROMARANGE  17
#define VKR_FOGCOLOR     18
#define VKR_ZACOLOR      19
#define VKR_STIPPLE      20
#define VKR_ALPHAREF     21
#define VKR_FOGTABLE     22     // lookup buffer word of the fog table (delta | blend << 8)
#define VKR_MISC         23     // fog delta mask | bilinear mask << 8
#define VKR_TMUCONFIG    24
#define VKR_START        25     // R, G, B, A (12.12), Z (20.12)
#define VKR_DDX          30     // their X gradients
#define VKR_DDY          35     // their Y gradients
#define VKR_W            40     // W start, X and Y gradients (16.32, 64-bit)
#define VKR_TMU0         46     // texture blocks, present when a texture is used
#define VKR_TMU1         (VKR_TMU0 + VKT_SIZE)
#define VKR_SIZE_NOTEX   46
#define VKR_SIZE_TEX     (VKR_TMU0 + 2 * VKT_SIZE)

// texture block
#define VKT_TEXMODE      0
#define VKT_LOOKUP       1      // lookup buffer word of the texel table
#define VKT_WMASK        2
#define VKT_HMASK        3
#define VKT_LODMIN       4
#define VKT_LODMAX       5
#define VKT_LODBIAS      6
#define VKT_LODMASK      7
#define VKT_DETAILMAX    8
#define VKT_DETAILBIAS   9
#define VKT_DETAILSCALE  10
#define VKT_RAMMASK      11
#define VKT_LODOFFSET    12     // 9 words
#define VKT_ST           21     // S, T, W start, X gradients, Y gradients (64-bit)
#define VKT_SIZE         39

// static texel tables at the start of the lookup buffer
#define VKL_RGB332       0
#define VKL_ALPHA8       256
#define VKL_INT8         512
#define VKL_AI44         768
#define VKL_RGB565       1024
#define VKL_ARGB1555     (1024 + 65536)
#define VKL_ARGB4444     (1024 + 2 * 65536)
#define VKL_STATIC_SIZE  (1024 + 3 * 65536)

// descriptor bindings
#define VKB_RECORDS      0
#define VKB_OFFSETS      1
#define VKB_LOOKUP       2
#define VKB_TMU0         3
#define VKB_TMU1         4
#define VKB_PIXELS       5
#define VKB_COUNT        6
