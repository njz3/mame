// license:BSD-3-Clause
// copyright-holders:njz3
//============================================================
//
//  sharedtexture.h - textures shared with the GPU renderers
//  of the emulation (mame3d)
//
//  With -hires3d, a GPU renderer of the emulation can copy its
//  frames into textures of the bgfx renderer instead of reading
//  them back: on Direct3D 11, the textures are shared through
//  NT handles and drawn like the screen textures.
//
//============================================================

#ifndef MAME_RENDER_BGFX_SHAREDTEXTURE_H
#define MAME_RENDER_BGFX_SHAREDTEXTURE_H

#pragma once

#include "texturehandleprovider.h"

#include <bgfx/bgfx.h>

#include <memory>


class bgfx_shared_textures
{
public:
	virtual ~bgfx_shared_textures() = default;

	// the textures of the running bgfx library, or nullptr (Direct3D 11 only)
	static bgfx_shared_textures *instance();

	// start once bgfx is initialized single-threaded (registers the sink with
	// hires3d_output: the textures are made and tracked from the emulation
	// thread, which must be bgfx's render thread), stop before bgfx shuts
	// down
	static void start();
	static void stop();

	// around bgfx::frame(): the textures made since the previous frame can
	// be drawn after it, the ones drawn in it are tracked
	virtual void before_frame() = 0;
	virtual void after_frame() = 0;

	// the texture holding a screen texture of width x height and rowpixels
	// (the frame at its top left corner), or nullptr if it cannot be drawn
	virtual bgfx_texture_handle_provider *provider(int id, uint16_t width, uint16_t height, uint16_t rowpixels) = 0;
};

#endif // MAME_RENDER_BGFX_SHAREDTEXTURE_H
