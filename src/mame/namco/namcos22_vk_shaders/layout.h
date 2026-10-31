// license:BSD-3-Clause
// copyright-holders:njz3
/***************************************************************************

    layout.h

    Data shared by namcos22_vk.cpp and its shaders, as 32-bit word offsets.

***************************************************************************/

// draw record of a polygon or sprite, what the scanline renderers of
// namcos22_v.cpp read from their object data
#define NR_KIND         0       // NRK_*
#define NR_PENBASE      1       // palette index of pen 0
#define NR_PENMASK      2
#define NR_PENSHIFT     3
#define NR_BANK         4       // texture bank * 0x1000
#define NR_FLAGS        5       // NRF_*
#define NR_FOGFACTOR    6       // blend factor of the colour against the fog (0xff: no fog)
#define NR_FOGCOLOR     7
#define NR_FADEFACTOR   8       // same against the fade colour
#define NR_FADECOLOR    9
#define NR_POLYCOLOR    10      // polygon fade scale
#define NR_ALPHA        11      // blend factor against the destination (0xff: opaque)
#define NR_ALPHAPEN     12
#define NR_PRIO         13      // prioverchar
#define NR_SOURCE       14      // sprite: first byte of the tile
#define NR_MODULO       15      // sprite: bytes per line
#define NR_FLIPX        16
#define NR_FLIPY        17
#define NR_SIZE         18

#define NRK_POLY        0       // System 22 polygon: fog, then shading
#define NRK_POLY_SUPER  1       // Super System 22 polygon: shading, fog, fades, alpha
#define NRK_SPRITE      2

#define NRF_SHADE       1
#define NRF_TEXTURE     2
#define NRF_ALPHA_ALL   4       // alpha on every pen, not only the alpha pen
#define NRF_POLYFADE    8

// vertex: x, y in target pixels, 4 parameters (1/z, u/z, v/z, i/z or
// sprite x, y), record word
#define NV_SIZE         8

// static texture data, in words
#define NT_TILEMAP      0           // 0x100000 u16
#define NT_TILEATTR     0x80000     // 0x100000 u8
#define NT_AYX          0xc0000     // 0x1000 u8
#define NT_TILEDATA     0xc0400     // u8

// mix parameters, in words
#define NM_SUPER        0
#define NM_BACKGROUND   1       // Super: what the text is mixed over before the scene
#define NM_FADE_ON      2
#define NM_FADE_R       3       // System 22: scale (0x100 = 1), Super: colour
#define NM_FADE_G       4
#define NM_FADE_B       5
#define NM_FADE_FACTOR  6       // Super: blend factor of the colour against the fade
#define NM_SHADOW_ON    7       // System 22
#define NM_MIX0         8       // System 22: shadow scales of pens 0xfc to 0xfe
#define NM_MIX1         9
#define NM_MIX2         10
#define NM_ALPHA_CHECK12 11     // Super: text alpha
#define NM_ALPHA_CHECK13 12
#define NM_ALPHA_MASK   13
#define NM_ALPHA_FACTOR 14
#define NM_SPOT_ON      15      // Super: spot
#define NM_SPOT_FACTOR  16
#define NM_SPOT_PALBASE 17
#define NM_TEXT_PALBASE 18
#define NM_SIZE         19

// passes of the mix shader
#define NP_SCENE        0
#define NP_TEXT_BEFORE  1       // Super: text over the background, before the scene
#define NP_FINAL        2       // text over the scene, fades, gamma

// descriptor bindings, set 0
#define NB_TEXTURE      0
#define NB_SPRITE       1
#define NB_FRAME        2
// set 1
#define NB_COLOR        0
#define NB_PRI          1
