#include "pch.hpp"

#include "imgui/custom/button_fx.hpp"

#include "shaders/button_fx_shaders.hpp"

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

// 按钮 SDF 特效。一次 Draw(6) 画一个外扩矩形，像素着色器里用圆角距离场
// 决定描边、内发光和外晕。混合是预乘：玻璃底按 alpha 盖住背景，光晕只加亮。

namespace button_fx
{
	namespace
	{
		constexpr int k_max_buttons = 16;

		struct alignas(16) button_constants
		{
			float rect[4];
			float accent[4];
			float state[4];
			float screen[4];
		};

		static_assert(sizeof(button_constants) == 64, "button_fx cbuffer layout mismatch");

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

			desc m_items[k_max_buttons] = {};
			int m_count = 0;
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

			std::cout << "[button_fx] shader compile failed: " << entry << "\n"
				<< static_cast<const char*>(error_blob->GetBufferPointer()) << '\n';
			error_blob->Release();
		}

		void draw_callback(const ImDrawList* parent_list, const ImDrawCmd* cmd)
		{
			(void)parent_list;
			(void)cmd;

			if (!g_fx.m_ready || g_fx.m_p_context == nullptr) return;

			const int index = static_cast<int>(reinterpret_cast<intptr_t>(cmd->UserCallbackData));
			if (index < 0 || index >= g_fx.m_count) return;

			const ImDrawData* draw_data = ImGui::GetDrawData();
			if (draw_data == nullptr) return;

			const desc& item = g_fx.m_items[index];
			const ImVec2 scale = draw_data->FramebufferScale;
			const ImVec2 origin = draw_data->DisplayPos;
			const float screen_w = draw_data->DisplaySize.x * scale.x;
			const float screen_h = draw_data->DisplaySize.y * scale.y;
			if (screen_w <= 1.0f || screen_h <= 1.0f) return;

			button_constants constants = {};
			constants.rect[0] = (item.m_min.x - origin.x) * scale.x;
			constants.rect[1] = (item.m_min.y - origin.y) * scale.y;
			constants.rect[2] = (item.m_max.x - origin.x) * scale.x;
			constants.rect[3] = (item.m_max.y - origin.y) * scale.y;
			constants.accent[0] = item.m_accent.x;
			constants.accent[1] = item.m_accent.y;
			constants.accent[2] = item.m_accent.z;
			constants.accent[3] = item.m_border * ((scale.x + scale.y) * 0.5f);
			constants.state[0] = item.m_hover;
			constants.state[1] = item.m_press;
			constants.state[2] = item.m_time;
			constants.state[3] = item.m_rounding * ((scale.x + scale.y) * 0.5f);
			constants.screen[0] = screen_w;
			constants.screen[1] = screen_h;
			constants.screen[2] = item.m_glow_pad * ((scale.x + scale.y) * 0.5f);
			constants.screen[3] = 0.0f;

			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (FAILED(g_fx.m_p_context->Map(g_fx.m_p_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return;

			memcpy(mapped.pData, &constants, sizeof(constants));
			g_fx.m_p_context->Unmap(g_fx.m_p_constants, 0);

			// 内容区的裁剪正好卡在按钮左右边上，外晕会被切掉。
			// 外扩小于面板内边距，光晕仍留在面板里，不必再跟命令裁剪求交。
			const float pad = constants.screen[2];
			D3D11_RECT scissor = {};
			scissor.left = static_cast<LONG>(std::floor(std::max(0.0f, constants.rect[0] - pad)));
			scissor.top = static_cast<LONG>(std::floor(std::max(0.0f, constants.rect[1] - pad)));
			scissor.right = static_cast<LONG>(std::ceil(std::min(screen_w, constants.rect[2] + pad)));
			scissor.bottom = static_cast<LONG>(std::ceil(std::min(screen_h, constants.rect[3] + pad)));
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
		HRESULT hr = D3DCompile(shader_source::k_button_fx, strlen(shader_source::k_button_fx),
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

		hr = D3DCompile(shader_source::k_button_fx, strlen(shader_source::k_button_fx),
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
		buffer_desc.ByteWidth = sizeof(button_constants);
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

		// 预乘：rgb 已经乘过覆盖率，光晕部分 alpha 为 0，只加亮不压暗背景。
		D3D11_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].BlendEnable = TRUE;
		blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
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
		g_fx.m_count = 0;
		g_fx.m_ready = false;
	}

	void begin_frame()
	{
		g_fx.m_count = 0;
	}

	bool submit(ImDrawList* draw_list, const desc& item)
	{
		if (!g_fx.m_ready || draw_list == nullptr) return false;
		if (g_fx.m_count >= k_max_buttons) return false;

		const int index = g_fx.m_count;
		g_fx.m_items[index] = item;
		g_fx.m_count += 1;

		draw_list->AddCallback(draw_callback, reinterpret_cast<void*>(static_cast<intptr_t>(index)));

		ImDrawCallback reset = ImGui::GetPlatformIO().DrawCallback_ResetRenderState;
		if (reset != nullptr)
			draw_list->AddCallback(reset, nullptr);

		return true;
	}
}
