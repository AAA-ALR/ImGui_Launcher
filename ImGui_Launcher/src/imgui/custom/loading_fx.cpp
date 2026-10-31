#include "pch.hpp"

#include "imgui/custom/loading_fx.hpp"

#include "shaders/loading_fx_shaders.hpp"

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

// 加载页彩虹竖条。一次 Draw(6) 盖住文字上下多出来的那一条，
// 像素着色器按间距放竖条，再乘上扫光包络。混合是加法。

namespace loading_fx
{
	namespace
	{
		struct alignas(16) loading_constants
		{
			float band[4];
			float sweep[4];
			float span[4];
			float screen[4];
		};

		static_assert(sizeof(loading_constants) == 64, "loading_fx cbuffer layout mismatch");

		struct runtime
		{
			ID3D11Device* m_p_device = nullptr;
			ID3D11DeviceContext* m_p_context = nullptr;
			ID3D11VertexShader* m_p_vertex = nullptr;
			ID3D11PixelShader* m_p_pixel = nullptr;
			ID3D11Buffer* m_p_constants = nullptr;
			ID3D11RasterizerState* m_p_raster = nullptr;
			ID3D11DepthStencilState* m_p_depth = nullptr;
			ID3D11BlendState* m_p_blend = nullptr;

			desc m_item = {};
			bool m_pending = false;
			bool m_ready = false;
		};

		runtime g_fx{};

		UINT compile_flags()
		{
			UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
			flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
			return flags;
		}

		void report_failure(const char* entry, ID3DBlob* error_blob)
		{
			if (error_blob == nullptr) return;

			std::cout << "[loading_fx] shader compile failed: " << entry << "\n"
				<< static_cast<const char*>(error_blob->GetBufferPointer()) << '\n';
			error_blob->Release();
		}

		void draw_callback(const ImDrawList* parent_list, const ImDrawCmd* cmd)
		{
			(void)parent_list;
			(void)cmd;

			if (!g_fx.m_ready || !g_fx.m_pending || g_fx.m_p_context == nullptr) return;

			const ImDrawData* draw_data = ImGui::GetDrawData();
			if (draw_data == nullptr) return;

			const desc& item = g_fx.m_item;
			const ImVec2 scale = draw_data->FramebufferScale;
			const ImVec2 origin = draw_data->DisplayPos;
			const float screen_w = draw_data->DisplaySize.x * scale.x;
			const float screen_h = draw_data->DisplaySize.y * scale.y;
			if (screen_w <= 1.0f || screen_h <= 1.0f) return;
			if (item.m_fade <= 0.001f) return;

			loading_constants constants = {};
			constants.band[0] = (item.m_min.x - origin.x) * scale.x;
			constants.band[1] = (item.m_min.y - origin.y) * scale.y;
			constants.band[2] = (item.m_max.x - origin.x) * scale.x;
			constants.band[3] = (item.m_max.y - origin.y) * scale.y;
			constants.sweep[0] = (item.m_sweep_x - origin.x) * scale.x;
			constants.sweep[1] = item.m_radius * ((scale.x + scale.y) * 0.5f);
			constants.sweep[2] = item.m_fade;
			constants.sweep[3] = item.m_step * ((scale.x + scale.y) * 0.5f);
			constants.span[0] = (item.m_text_left - origin.x) * scale.x;
			constants.span[1] = (item.m_text_right - origin.x) * scale.x;
			constants.screen[0] = screen_w;
			constants.screen[1] = screen_h;

			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (FAILED(g_fx.m_p_context->Map(g_fx.m_p_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return;

			memcpy(mapped.pData, &constants, sizeof(constants));
			g_fx.m_p_context->Unmap(g_fx.m_p_constants, 0);

			D3D11_RECT scissor = {};
			scissor.left = static_cast<LONG>(std::floor(std::max(0.0f, constants.band[0])));
			scissor.top = static_cast<LONG>(std::floor(std::max(0.0f, constants.band[1])));
			scissor.right = static_cast<LONG>(std::ceil(std::min(screen_w, constants.band[2])));
			scissor.bottom = static_cast<LONG>(std::ceil(std::min(screen_h, constants.band[3])));
			if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) return;

			ID3D11Buffer* no_buffer = nullptr;
			const UINT zero = 0;
			const float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

			g_fx.m_p_context->IASetInputLayout(nullptr);
			g_fx.m_p_context->IASetVertexBuffers(0, 1, &no_buffer, &zero, &zero);
			g_fx.m_p_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			g_fx.m_p_context->VSSetShader(g_fx.m_p_vertex, nullptr, 0);
			g_fx.m_p_context->VSSetConstantBuffers(0, 1, &g_fx.m_p_constants);
			g_fx.m_p_context->PSSetShader(g_fx.m_p_pixel, nullptr, 0);
			g_fx.m_p_context->PSSetConstantBuffers(0, 1, &g_fx.m_p_constants);
			g_fx.m_p_context->RSSetState(g_fx.m_p_raster);
			g_fx.m_p_context->RSSetScissorRects(1, &scissor);
			g_fx.m_p_context->OMSetDepthStencilState(g_fx.m_p_depth, 0);
			g_fx.m_p_context->OMSetBlendState(g_fx.m_p_blend, blend_factor, 0xffffffff);
			g_fx.m_p_context->Draw(6, 0);
		}
	}

	bool init(ID3D11Device* device, ID3D11DeviceContext* context)
	{
		if (device == nullptr || context == nullptr) return false;
		if (g_fx.m_ready) return true;

		g_fx.m_p_device = device;
		g_fx.m_p_context = context;

		ID3DBlob* shader_blob = nullptr;
		ID3DBlob* error_blob = nullptr;
		HRESULT hr = D3DCompile(shader_source::k_loading_fx, strlen(shader_source::k_loading_fx),
			nullptr, nullptr, nullptr, "vs_main", "vs_5_0", compile_flags(), 0, &shader_blob, &error_blob);
		if (FAILED(hr))
		{
			report_failure("vs_main", error_blob);
			shutdown();
			return false;
		}

		if (error_blob != nullptr) error_blob->Release();

		hr = device->CreateVertexShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(), nullptr, &g_fx.m_p_vertex);
		shader_blob->Release();
		shader_blob = nullptr;
		if (FAILED(hr))
		{
			shutdown();
			return false;
		}

		hr = D3DCompile(shader_source::k_loading_fx, strlen(shader_source::k_loading_fx),
			nullptr, nullptr, nullptr, "ps_main", "ps_5_0", compile_flags(), 0, &shader_blob, &error_blob);
		if (FAILED(hr))
		{
			report_failure("ps_main", error_blob);
			shutdown();
			return false;
		}

		if (error_blob != nullptr) error_blob->Release();

		hr = device->CreatePixelShader(shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(), nullptr, &g_fx.m_p_pixel);
		shader_blob->Release();
		if (FAILED(hr))
		{
			shutdown();
			return false;
		}

		D3D11_BUFFER_DESC buffer_desc = {};
		buffer_desc.ByteWidth = sizeof(loading_constants);
		buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
		buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &g_fx.m_p_constants)))
		{
			shutdown();
			return false;
		}

		D3D11_RASTERIZER_DESC raster_desc = {};
		raster_desc.FillMode = D3D11_FILL_SOLID;
		raster_desc.CullMode = D3D11_CULL_NONE;
		raster_desc.DepthClipEnable = TRUE;
		raster_desc.ScissorEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&raster_desc, &g_fx.m_p_raster)))
		{
			shutdown();
			return false;
		}

		D3D11_DEPTH_STENCIL_DESC depth_desc = {};
		depth_desc.DepthEnable = FALSE;
		depth_desc.StencilEnable = FALSE;
		if (FAILED(device->CreateDepthStencilState(&depth_desc, &g_fx.m_p_depth)))
		{
			shutdown();
			return false;
		}

		D3D11_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].BlendEnable = TRUE;
		blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blend_desc, &g_fx.m_p_blend)))
		{
			shutdown();
			return false;
		}

		g_fx.m_ready = true;
		return true;
	}

	void shutdown()
	{
		if (g_fx.m_p_blend) { g_fx.m_p_blend->Release(); g_fx.m_p_blend = nullptr; }
		if (g_fx.m_p_depth) { g_fx.m_p_depth->Release(); g_fx.m_p_depth = nullptr; }
		if (g_fx.m_p_raster) { g_fx.m_p_raster->Release(); g_fx.m_p_raster = nullptr; }
		if (g_fx.m_p_constants) { g_fx.m_p_constants->Release(); g_fx.m_p_constants = nullptr; }
		if (g_fx.m_p_pixel) { g_fx.m_p_pixel->Release(); g_fx.m_p_pixel = nullptr; }
		if (g_fx.m_p_vertex) { g_fx.m_p_vertex->Release(); g_fx.m_p_vertex = nullptr; }

		g_fx.m_p_device = nullptr;
		g_fx.m_p_context = nullptr;
		g_fx.m_pending = false;
		g_fx.m_ready = false;
	}

	void begin_frame()
	{
		g_fx.m_pending = false;
	}

	bool submit(ImDrawList* draw_list, const desc& item)
	{
		if (!g_fx.m_ready || draw_list == nullptr) return false;
		if (item.m_fade <= 0.001f) return true;

		g_fx.m_item = item;
		g_fx.m_pending = true;

		draw_list->AddCallback(draw_callback, nullptr);

		ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
		if (reset != nullptr)
			draw_list->AddCallback(reset, nullptr);

		return true;
	}
}
