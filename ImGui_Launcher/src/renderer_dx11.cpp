#include "pch.hpp"

#include "renderer_dx11.hpp"

namespace renderer
{
	bool create(HWND hwnd, int width, int height)
	{
		if (g_state.m_initialized) return true;

		D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };

		const HRESULT hr = D3D11CreateDeviceAndSwapChain(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT,
			feature_levels,
			static_cast<UINT>(std::size(feature_levels)),
			D3D11_SDK_VERSION,
			nullptr,
			nullptr,
			&g_state.m_p_device,
			nullptr,
			&g_state.m_p_context);

		if (FAILED(hr)) return false;

		// 用 DXGI 工厂单独创建 flip-model 交换链（D3D11CreateDeviceAndSwapChain 不支持 FLIP_DISCARD）
		DXGI_SWAP_CHAIN_DESC1 desc = {};
		desc.Width = static_cast<UINT>(width);
		desc.Height = static_cast<UINT>(height);
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		desc.BufferCount = 2;
		desc.Scaling = DXGI_SCALING_STRETCH;
		desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		IDXGIDevice* dxgi_device = nullptr;
		IDXGIAdapter* adapter = nullptr;
		IDXGIFactory2* factory = nullptr;

		if (FAILED(g_state.m_p_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) ||
			FAILED(dxgi_device->GetAdapter(&adapter)) ||
			FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))))
		{
			if (dxgi_device) dxgi_device->Release();
			if (adapter) adapter->Release();
			return false;
		}

		const HRESULT swap_hr = factory->CreateSwapChainForHwnd(
			g_state.m_p_device,
			hwnd,
			&desc,
			nullptr,
			nullptr,
			reinterpret_cast<IDXGISwapChain1**>(&g_state.m_p_swap_chain));

		factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

		factory->Release();
		adapter->Release();
		dxgi_device->Release();

		if (FAILED(swap_hr)) return false;

		g_state.m_initialized = true;
		return true;
	}

	void resize(int width, int height)
	{
		if (!g_state.m_initialized || width <= 0 || height <= 0) return;

		if (g_state.m_p_render_target)
		{
			g_state.m_p_render_target->Release();
			g_state.m_p_render_target = nullptr;
		}

		g_state.m_p_swap_chain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0);
	}

	void destroy()
	{
		if (g_state.m_p_swap_chain)
			g_state.m_p_swap_chain->SetFullscreenState(FALSE, nullptr);

		if (g_state.m_p_render_target) { g_state.m_p_render_target->Release(); g_state.m_p_render_target = nullptr; }
		if (g_state.m_p_depth_stencil) { g_state.m_p_depth_stencil->Release(); g_state.m_p_depth_stencil = nullptr; }
		if (g_state.m_p_swap_chain) { g_state.m_p_swap_chain->Release(); g_state.m_p_swap_chain = nullptr; }
		if (g_state.m_p_context) { g_state.m_p_context->ClearState(); g_state.m_p_context->Flush(); g_state.m_p_context->Release(); g_state.m_p_context = nullptr; }
		if (g_state.m_p_device) { g_state.m_p_device->Release(); g_state.m_p_device = nullptr; }

		g_state.m_initialized = false;
	}

	void begin_frame(float clear_color[4])
	{
		if (!g_state.m_initialized) return;

		// 每帧从交换链重新拿 back buffer：ResizeBuffers 之后旧 RTV 失效
		if (g_state.m_p_render_target == nullptr)
		{
			ID3D11Texture2D* back_buffer = nullptr;
			if (SUCCEEDED(g_state.m_p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))) && back_buffer != nullptr)
			{
				g_state.m_p_device->CreateRenderTargetView(back_buffer, nullptr, &g_state.m_p_render_target);
				back_buffer->Release();
			}
		}

		ID3D11RenderTargetView* targets[1] = { g_state.m_p_render_target };
		g_state.m_p_context->OMSetRenderTargets(1, targets, g_state.m_p_depth_stencil);
		g_state.m_p_context->ClearRenderTargetView(g_state.m_p_render_target, clear_color);
	}

	void end_frame()
	{
		if (!g_state.m_initialized) return;

		// vsync 开启，占用低；要跑满帧率改成 0
		g_state.m_p_swap_chain->Present(1, 0);
	}

	void set_occluded(bool occluded)
	{
		g_state.m_occluded = occluded;
	}
}
