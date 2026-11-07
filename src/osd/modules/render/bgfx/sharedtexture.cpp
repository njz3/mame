// license:BSD-3-Clause
// copyright-holders:njz3
//============================================================
//
//  sharedtexture.cpp - textures shared with the GPU renderers
//  of the emulation (mame3d)
//
//  bgfx draws on the emulation thread then (single-threaded):
//  its Direct3D 11 device and context are only used from there.
//  Each texture is a Direct3D 11 texture (a shader resource)
//  created with an NT handle that the GPU renderer imports, and
//  a bgfx texture overridden with it once bgfx made its own
//  (after a frame).  The GPU renderer waits for its copy before
//  the frame is drawn, and asks whether the draws that read a
//  texture are done before it writes it again: each frame that
//  draws shared textures ends with an event query of a ring, and
//  a frame is done once its query or a later one is (the GPU
//  runs the frames in order).
//
//============================================================

#include "sharedtexture.h"

#include "emu.h"
#include "hires3d.h"

#include <bgfx/platform.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#endif


namespace {

bgfx_shared_textures *s_instance = nullptr;


// a texture as a screen texture of the chains
class shared_provider : public bgfx_texture_handle_provider
{
public:
	bgfx::TextureHandle m_handle = BGFX_INVALID_HANDLE;
	uint16_t m_width = 0;
	uint16_t m_height = 0;
	uint16_t m_rowpixels = 0;

	virtual bgfx::TextureHandle texture() const override { return m_handle; }
	virtual bool is_target() const override { return false; }
	virtual uint16_t width() const override { return m_width; }
	virtual uint16_t width_margin() const override { return 0; }
	virtual uint16_t height() const override { return m_height; }
	virtual uint16_t rowpixels() const override { return m_rowpixels; }
	virtual int width_div_factor() const override { return 1; }
	virtual int width_mul_factor() const override { return 1; }
};


#if defined(_WIN32)

class shared_d3d11 : public bgfx_shared_textures, public hires3d_gpu_sink
{
public:
	shared_d3d11(ID3D11Device *device)
		: m_device(device)
	{
		m_device->AddRef();
		m_device->GetImmediateContext(&m_context);
		D3D11_QUERY_DESC const query{ D3D11_QUERY_EVENT, 0 };
		for (ID3D11Query *&q : m_queries)
			m_device->CreateQuery(&query, &q);
		IDXGIDevice *dxgi = nullptr;
		if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi))))
		{
			IDXGIAdapter *adapter = nullptr;
			if (SUCCEEDED(dxgi->GetAdapter(&adapter)))
			{
				DXGI_ADAPTER_DESC desc;
				if (SUCCEEDED(adapter->GetDesc(&desc)))
					m_luid = desc.AdapterLuid;
				adapter->Release();
			}
			dxgi->Release();
		}
	}

	virtual ~shared_d3d11()
	{
		for (auto &item : m_entries)
			release(item.second);
		for (ID3D11Query *q : m_queries)
			if (q)
				q->Release();
		if (m_context)
			m_context->Release();
		m_device->Release();
	}

	// hires3d_gpu_sink
	virtual void adapter_luid(u8 *luid) const override
	{
		std::memcpy(luid, &m_luid, sizeof(m_luid));
	}

	virtual int create(int width, int height, void *&handle) override
	{
		handle = nullptr;
		if ((width <= 0) || (height <= 0) || (width > 16384) || (height > 16384))
			return 0;
		entry e;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		// black, and initialized on the GPU before the GPU renderer gets it
		// (it writes the texture from its own queue)
		std::vector<uint32_t> const black(size_t(width) * height, 0xff000000);
		D3D11_SUBRESOURCE_DATA const initial{ black.data(), UINT(width * 4), 0 };
		if (FAILED(m_device->CreateTexture2D(&desc, &initial, &e.texture)))
			return 0;
		IDXGIResource1 *resource = nullptr;
		bool ok = SUCCEEDED(e.texture->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&resource)));
		if (ok)
		{
			ok = SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &e.handle));
			resource->Release();
		}
		if (!ok)
		{
			release(e);
			return 0;
		}
		finish();

		// bgfx makes its texture at the next frame, and it is overridden then
		uint64_t const flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
		e.bgfx = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::BGRA8, flags);
		e.width = uint16_t(width);
		e.height = uint16_t(height);
		int const id = m_next_id++;
		handle = e.handle;
		m_entries.emplace(id, e);
		return id;
	}

	virtual bool ready(int id) const override
	{
		auto const found = m_entries.find(id);
		return (found != m_entries.end()) && found->second.ready;
	}

	virtual void written(int id) override
	{
		auto const found = m_entries.find(id);
		if (found != m_entries.end())
			found->second.drawable = true;
	}

	virtual bool idle(int id, bool wait) override
	{
		// the GPU renderer writes the texture again: it is not drawable
		// until written(), and the draws that used it must be done
		auto const found = m_entries.find(id);
		if (found == m_entries.end())
			return true;
		entry &e = found->second;
		if (!done(e.drawn_frame, wait))
			return false;
		e.drawable = false;
		return true;
	}

	virtual void destroy(int id) override
	{
		auto const found = m_entries.find(id);
		if (found == m_entries.end())
			return;
		release(found->second);
		m_entries.erase(found);
	}

	// bgfx_shared_textures
	virtual void before_frame() override
	{
		for (auto &item : m_entries)
		{
			entry &e = item.second;
			if (!e.ready && bgfx::isValid(e.bgfx) && bgfx::overrideInternal(e.bgfx, uintptr_t(e.texture)))
				e.ready = true;
		}
	}

	virtual void after_frame() override
	{
		// the frame that drew shared textures ends with a query
		bool drawn = false;
		for (auto &item : m_entries)
			drawn = drawn || item.second.drawn;
		if (!drawn)
			return;
		m_frame++;
		int const index = int(m_frame % QUERIES);
		if (m_queries[index])
		{
			m_context->End(m_queries[index]);
			m_query_frame[index] = m_frame;
		}
		for (auto &item : m_entries)
		{
			entry &e = item.second;
			if (e.drawn)
				e.drawn_frame = m_frame;
			e.drawn = false;
		}
	}

	virtual bgfx_texture_handle_provider *provider(int id, uint16_t width, uint16_t height, uint16_t rowpixels) override
	{
		auto const found = m_entries.find(id);
		if (found == m_entries.end())
			return nullptr;
		entry &e = found->second;
		if (!e.ready || !e.drawable || (e.width != rowpixels) || (e.height != height) || (width > rowpixels))
			return nullptr;
		e.provider.m_handle = e.bgfx;
		e.provider.m_width = width;
		e.provider.m_height = height;
		e.provider.m_rowpixels = rowpixels;
		e.drawn = true;
		return &e.provider;
	}

private:
	struct entry
	{
		ID3D11Texture2D *texture = nullptr;
		HANDLE handle = nullptr;
		bgfx::TextureHandle bgfx = BGFX_INVALID_HANDLE;
		uint16_t width = 0;
		uint16_t height = 0;
		bool ready = false;     // the bgfx texture is overridden
		bool drawable = false;  // holds a frame the GPU renderer wrote
		bool drawn = false;     // drawn in the frame being built
		u64 drawn_frame = 0;    // last frame that drew it, 0 if none
		shared_provider provider;
	};

	// wait until the GPU is done with everything sent so far (a second at most)
	void finish()
	{
		ID3D11Query *query = nullptr;
		D3D11_QUERY_DESC const desc{ D3D11_QUERY_EVENT, 0 };
		if (FAILED(m_device->CreateQuery(&desc, &query)))
			return;
		m_context->End(query);
		m_context->Flush();
		osd_ticks_t const start = osd_ticks();
		while ((m_context->GetData(query, nullptr, 0, 0) == S_FALSE) && (osd_ticks() - start < osd_ticks_per_second()))
			std::this_thread::yield();
		query->Release();
	}

	// true once the draws of frame are done on the GPU (with wait, waits
	// for them, a second at most)
	bool done(u64 frame, bool wait)
	{
		osd_ticks_t const start = osd_ticks();
		while (m_done < frame)
		{
			for (int i = 0; i < QUERIES; i++)
				if (m_queries[i] && (m_query_frame[i] > m_done) && (m_context->GetData(m_queries[i], nullptr, 0, wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK))
					m_done = std::max(m_done, m_query_frame[i]);
			if (m_done >= frame)
				break;
			if (!wait)
				return false;
			if (osd_ticks() - start > osd_ticks_per_second())
				return true;
			std::this_thread::yield();
		}
		return true;
	}
	void release(entry &e)
	{
		// bgfx keeps its view of the texture (and a reference) until its
		// texture is destroyed, at the next frame
		if (bgfx::isValid(e.bgfx))
			bgfx::destroy(e.bgfx);
		if (e.texture)
			e.texture->Release();
		if (e.handle)
			CloseHandle(e.handle);
		e = entry();
	}

	ID3D11Device *m_device;
	ID3D11DeviceContext *m_context = nullptr;
	LUID m_luid{};
	std::map<int, entry> m_entries;
	int m_next_id = 1;
	static constexpr int QUERIES = 8;
	ID3D11Query *m_queries[QUERIES]{};
	u64 m_query_frame[QUERIES]{};   // frame each query ends
	u64 m_frame = 0;                // frames that drew shared textures
	u64 m_done = 0;                 // the last of them known done on the GPU
};

std::unique_ptr<shared_d3d11> s_d3d11;

#endif // defined(_WIN32)

} // anonymous namespace


bgfx_shared_textures *bgfx_shared_textures::instance()
{
	return s_instance;
}


void bgfx_shared_textures::start()
{
	stop();
#if defined(_WIN32)
	if (bgfx::getRendererType() == bgfx::RendererType::Direct3D11)
	{
		auto *const device = reinterpret_cast<ID3D11Device *>(bgfx::getInternalData()->context);
		if (device)
		{
			s_d3d11 = std::make_unique<shared_d3d11>(device);
			s_instance = s_d3d11.get();
			hires3d_output::set_gpu_sink(s_d3d11.get());
			osd_printf_verbose("BGFX: frames of the GPU renderers can be drawn without read-back\n");
		}
	}
#endif
}


void bgfx_shared_textures::stop()
{
#if defined(_WIN32)
	if (s_d3d11)
	{
		hires3d_output::set_gpu_sink(nullptr);
		s_instance = nullptr;
		s_d3d11.reset();
	}
#endif
}
