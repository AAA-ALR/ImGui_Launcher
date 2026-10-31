#include "pch.hpp"

#include "effects/backdrop_blur.hpp"

#include "renderer_dx11.hpp"
#include "shaders/hlsl_source.hpp"

#include <d3dcompiler.h>

namespace backdrop_blur
{
	namespace
	{
		// Gaussian radius in 1/4-resolution texels. The 9-tap kernel reaches
		// roughly 4 * k_blur_radius texels per side, i.e. 4 * k_blur_radius * 4
		// full-resolution pixels. 1.6 gives ~26px: a solid frosted panel that
		// still keeps the light streaks readable as shapes.
		constexpr float k_blur_radius = 1.6f;

		// Downsample factor for the intermediate targets.
		constexpr int k_downsample = 4;

		struct alignas(16) backdrop_constants
		{
			float texel[4];  // xy = size, zw = 1 / size
			float step[4];   // xy = uv step, z = radius scale, w = feather width
			float rect[4];   // xy = top-left, zw = size
			float round[4];  // x = corner radius in pixels
		};

		static_assert(sizeof(backdrop_constants) == 64, "backdrop cbuffer layout mismatch");

		struct blur_target
		{
			ID3D11Texture2D* m_p_texture = nullptr;
			ID3D11RenderTargetView* m_p_rtv = nullptr;
			ID3D11ShaderResourceView* m_p_srv = nullptr;
			int m_width = 0;
			int m_height = 0;
		};

		struct blur_runtime
		{
			ID3D11Device* m_p_device = nullptr;
			ID3D11DeviceContext* m_p_context = nullptr;

			ID3D11VertexShader* m_p_vertex = nullptr;
			ID3D11PixelShader* m_p_blur = nullptr;
			ID3D11PixelShader* m_p_composite = nullptr;
			ID3D11Buffer* m_p_constants = nullptr;

			ID3D11SamplerState* m_p_sampler = nullptr;
			ID3D11RasterizerState* m_p_raster = nullptr;
			ID3D11BlendState* m_p_blend = nullptr;
			ID3D11DepthStencilState* m_p_depth = nullptr;

			// Copy of the back buffer, used as the blur input.
			ID3D11Texture2D* m_p_capture = nullptr;
			ID3D11ShaderResourceView* m_p_capture_srv = nullptr;

			// Intermediate targets for the horizontal and vertical passes.
			blur_target m_temp;
			blur_target m_half;

			int m_width = 0;
			int m_height = 0;
			float m_strength = 1.0f;
			bool m_ready = false;
		};

		blur_runtime g_blur{};

		void release_target(blur_target& target)
		{
			if (target.m_p_srv) { target.m_p_srv->Release(); target.m_p_srv = nullptr; }
			if (target.m_p_rtv) { target.m_p_rtv->Release(); target.m_p_rtv = nullptr; }
			if (target.m_p_texture) { target.m_p_texture->Release(); target.m_p_texture = nullptr; }
			target.m_width = 0;
			target.m_height = 0;
		}

		bool create_target(blur_target& target, ID3D11Device* device, int width, int height)
		{
			release_target(target);

			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = static_cast<UINT>(width);
			desc.Height = static_cast<UINT>(height);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			// Same format as the back buffer so the copy is lossless.
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

			if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.m_p_texture))) return false;
			if (FAILED(device->CreateRenderTargetView(target.m_p_texture, nullptr, &target.m_p_rtv))) return false;

			D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.Format = desc.Format;
			srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MipLevels = 1;
			if (FAILED(device->CreateShaderResourceView(target.m_p_texture, &srv_desc, &target.m_p_srv))) return false;

			target.m_width = width;
			target.m_height = height;
			return true;
		}

		bool create_capture(ID3D11Device* device, int width, int height)
		{
			if (g_blur.m_p_capture_srv) { g_blur.m_p_capture_srv->Release(); g_blur.m_p_capture_srv = nullptr; }
			if (g_blur.m_p_capture) { g_blur.m_p_capture->Release(); g_blur.m_p_capture = nullptr; }

			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = static_cast<UINT>(width);
			desc.Height = static_cast<UINT>(height);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_blur.m_p_capture))) return false;

			D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
			srv_desc.Format = desc.Format;
			srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv_desc.Texture2D.MipLevels = 1;
			return SUCCEEDED(device->CreateShaderResourceView(g_blur.m_p_capture, &srv_desc, &g_blur.m_p_capture_srv));
		}

		bool compile(const char* source, const char* entry, ID3DBlob** out_blob)
		{
			UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR;
#ifdef _DEBUG
			flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

			ID3DBlob* error_blob = nullptr;
			const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
				entry, "ps_5_0", flags, 0, out_blob, &error_blob);

			if (FAILED(hr))
			{
				if (error_blob != nullptr)
				{
					std::cout << "[blur] shader compile failed: " << entry << "\n"
						<< static_cast<const char*>(error_blob->GetBufferPointer()) << '\n';
					error_blob->Release();
				}
				return false;
			}

			if (error_blob != nullptr) error_blob->Release();
			return true;
		}

		bool create_pixel_shader(const char* source, const char* entry, ID3D11PixelShader** out_shader)
		{
			ID3DBlob* blob = nullptr;
			if (!compile(source, entry, &blob)) return false;

			const HRESULT hr = g_blur.m_p_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_shader);
			blob->Release();
			return SUCCEEDED(hr);
		}

		void update_constants(const backdrop_constants& data)
		{
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (SUCCEEDED(g_blur.m_p_context->Map(g_blur.m_p_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				memcpy(mapped.pData, &data, sizeof(data));
				g_blur.m_p_context->Unmap(g_blur.m_p_constants, 0);
			}
		}

		void draw_fullscreen()
		{
			g_blur.m_p_context->Draw(3, 0);
		}
	}

	bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
	{
		if (device == nullptr || context == nullptr || width <= 0 || height <= 0) return false;
		if (g_blur.m_ready) return true;

		g_blur.m_p_device = device;
		g_blur.m_p_context = context;
		g_blur.m_width = width;
		g_blur.m_height = height;

		ID3DBlob* vertex_blob = nullptr;
		{
			UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
			flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
			ID3DBlob* error_blob = nullptr;
			if (FAILED(D3DCompile(shader_source::k_fullscreen, strlen(shader_source::k_fullscreen),
				nullptr, nullptr, nullptr, "vs_main", "vs_5_0", flags, 0, &vertex_blob, &error_blob)))
			{
				if (error_blob != nullptr) error_blob->Release();
				return false;
			}
			if (error_blob != nullptr) error_blob->Release();
		}

		const HRESULT vertex_hr = device->CreateVertexShader(
			vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr, &g_blur.m_p_vertex);
		vertex_blob->Release();
		if (FAILED(vertex_hr)) return false;

		if (!create_pixel_shader(shader_source::k_backdrop, "ps_blur", &g_blur.m_p_blur)) return false;
		if (!create_pixel_shader(shader_source::k_backdrop, "ps_composite", &g_blur.m_p_composite)) return false;

		D3D11_BUFFER_DESC buffer_desc = {};
		buffer_desc.ByteWidth = sizeof(backdrop_constants);
		buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
		buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &g_blur.m_p_constants))) return false;

		D3D11_SAMPLER_DESC sampler_desc = {};
		sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler_desc.MinLOD = 0.0f;
		sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampler_desc, &g_blur.m_p_sampler))) return false;

		D3D11_RASTERIZER_DESC raster_desc = {};
		raster_desc.FillMode = D3D11_FILL_SOLID;
		raster_desc.CullMode = D3D11_CULL_NONE;
		raster_desc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&raster_desc, &g_blur.m_p_raster))) return false;

		// Premultiplied alpha: ps_composite outputs rgb * mask and alpha = mask,
		// so ONE / INV_SRC_ALPHA blends it over the back buffer directly.
		D3D11_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].BlendEnable = TRUE;
		blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blend_desc, &g_blur.m_p_blend))) return false;

		D3D11_DEPTH_STENCIL_DESC depth_desc = {};
		depth_desc.DepthEnable = FALSE;
		depth_desc.StencilEnable = FALSE;
		if (FAILED(device->CreateDepthStencilState(&depth_desc, &g_blur.m_p_depth))) return false;

		const int half_w = std::max(1, width / k_downsample);
		const int half_h = std::max(1, height / k_downsample);

		if (!create_capture(device, width, height)) return false;
		if (!create_target(g_blur.m_temp, device, half_w, half_h)) return false;
		if (!create_target(g_blur.m_half, device, half_w, half_h)) return false;

		g_blur.m_ready = true;
		return true;
	}

	void shutdown()
	{
		if (!g_blur.m_ready && g_blur.m_p_device == nullptr) return;

		release_target(g_blur.m_temp);
		release_target(g_blur.m_half);

		if (g_blur.m_p_capture_srv) { g_blur.m_p_capture_srv->Release(); g_blur.m_p_capture_srv = nullptr; }
		if (g_blur.m_p_capture) { g_blur.m_p_capture->Release(); g_blur.m_p_capture = nullptr; }
		if (g_blur.m_p_blur) { g_blur.m_p_blur->Release(); g_blur.m_p_blur = nullptr; }
		if (g_blur.m_p_composite) { g_blur.m_p_composite->Release(); g_blur.m_p_composite = nullptr; }
		if (g_blur.m_p_vertex) { g_blur.m_p_vertex->Release(); g_blur.m_p_vertex = nullptr; }
		if (g_blur.m_p_constants) { g_blur.m_p_constants->Release(); g_blur.m_p_constants = nullptr; }
		if (g_blur.m_p_sampler) { g_blur.m_p_sampler->Release(); g_blur.m_p_sampler = nullptr; }
		if (g_blur.m_p_raster) { g_blur.m_p_raster->Release(); g_blur.m_p_raster = nullptr; }
		if (g_blur.m_p_blend) { g_blur.m_p_blend->Release(); g_blur.m_p_blend = nullptr; }
		if (g_blur.m_p_depth) { g_blur.m_p_depth->Release(); g_blur.m_p_depth = nullptr; }

		g_blur.m_p_device = nullptr;
		g_blur.m_p_context = nullptr;
		g_blur.m_ready = false;
	}

	void set_strength(float strength)
	{
		g_blur.m_strength = std::clamp(strength, 0.0f, 4.0f);
	}

	void render(float x, float y, float width, float height, float corner_radius)
	{
		if (!g_blur.m_ready) return;
		if (width <= 0.0f || height <= 0.0f) return;

		ID3D11Texture2D* back_buffer = nullptr;
		if (FAILED(renderer::g_state.m_p_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer))))
			return;

		ID3D11DeviceContext* context = g_blur.m_p_context;

		// Copy the whole back buffer once: it is write-only, so it cannot be sampled.
		context->CopyResource(g_blur.m_p_capture, back_buffer);
		back_buffer->Release();

		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_blur.m_p_vertex, nullptr, 0);
		context->RSSetState(g_blur.m_p_raster);
		context->OMSetDepthStencilState(g_blur.m_p_depth, 0);

		ID3D11SamplerState* samplers[1] = { g_blur.m_p_sampler };
		context->PSSetSamplers(0, 1, samplers);

		ID3D11Buffer* buffers[1] = { g_blur.m_p_constants };
		context->PSSetConstantBuffers(0, 1, buffers);

		D3D11_VIEWPORT viewport = {};
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;

		backdrop_constants constants = {};
		const float texel_u = 1.0f / static_cast<float>(g_blur.m_width);
		const float texel_v = 1.0f / static_cast<float>(g_blur.m_height);

		// ---- 1. horizontal blur: capture -> half ----
		// uv 约定：每个 pass 都按「目标像素坐标 / 目标尺寸」归一化，
		// 这样目标纹理始终代表整屏，源图 0..1 的采样也覆盖整屏。
		// 之前这里写的是 1 / 源尺寸，uv 只跑到 0.25，等于只把源图左上角
		// 那一小块放大铺满，合成后面板里就只剩一条横带模糊。
		constants.texel[0] = static_cast<float>(g_blur.m_half.m_width);
		constants.texel[1] = static_cast<float>(g_blur.m_half.m_height);
		constants.texel[2] = 1.0f / static_cast<float>(g_blur.m_half.m_width);
		constants.texel[3] = 1.0f / static_cast<float>(g_blur.m_half.m_height);
		constants.step[0] = k_blur_radius * k_downsample * g_blur.m_strength * texel_u;
		constants.step[1] = 0.0f;
		constants.step[2] = 1.0f;
		constants.step[3] = 1.0f;
		update_constants(constants);

		ID3D11RenderTargetView* half_rtv[1] = { g_blur.m_half.m_p_rtv };
		context->OMSetRenderTargets(1, half_rtv, nullptr);
		viewport.Width = static_cast<float>(g_blur.m_half.m_width);
		viewport.Height = static_cast<float>(g_blur.m_half.m_height);
		context->RSSetViewports(1, &viewport);

		ID3D11ShaderResourceView* capture_srv[1] = { g_blur.m_p_capture_srv };
		context->PSSetShaderResources(0, 1, capture_srv);
		context->PSSetShader(g_blur.m_p_blur, nullptr, 0);
		draw_fullscreen();

		ID3D11ShaderResourceView* null_srv[1] = { nullptr };
		context->PSSetShaderResources(0, 1, null_srv);

		// ---- 2. vertical blur: half -> temp ----
		constants.texel[0] = static_cast<float>(g_blur.m_half.m_width);
		constants.texel[1] = static_cast<float>(g_blur.m_half.m_height);
		constants.texel[2] = 1.0f / static_cast<float>(g_blur.m_half.m_width);
		constants.texel[3] = 1.0f / static_cast<float>(g_blur.m_half.m_height);
		constants.step[0] = 0.0f;
		constants.step[1] = k_blur_radius * k_downsample * g_blur.m_strength /
			static_cast<float>(g_blur.m_height);
		update_constants(constants);

		ID3D11RenderTargetView* temp_rtv[1] = { g_blur.m_temp.m_p_rtv };
		context->OMSetRenderTargets(1, temp_rtv, nullptr);
		viewport.Width = static_cast<float>(g_blur.m_temp.m_width);
		viewport.Height = static_cast<float>(g_blur.m_temp.m_height);
		context->RSSetViewports(1, &viewport);

		ID3D11ShaderResourceView* half_srv[1] = { g_blur.m_half.m_p_srv };
		context->PSSetShaderResources(0, 1, half_srv);
		context->PSSetShader(g_blur.m_p_blur, nullptr, 0);
		draw_fullscreen();

		context->PSSetShaderResources(0, 1, null_srv);

		// ---- 3. composite back onto the back buffer, clipped to the panel rect ----
		// 这里画到的是整块 back buffer，所以 uv 必须按 back buffer 尺寸归一化。
		// 之前写的是 1 / temp 尺寸，uv 跑到 0..4，超出 1 的部分被 CLAMP 采样器
		// 拉成最后一行的颜色往下抹，面板里就出现「上面一条模糊、下面一片死色」。
		constants.texel[0] = static_cast<float>(g_blur.m_width);
		constants.texel[1] = static_cast<float>(g_blur.m_height);
		constants.texel[2] = texel_u;
		constants.texel[3] = texel_v;
		constants.step[0] = 0.0f;
		constants.step[1] = 0.0f;
		constants.step[2] = 1.0f;
		constants.step[3] = 1.0f; // 1px feather
		constants.rect[0] = x;
		constants.rect[1] = y;
		constants.rect[2] = width;
		constants.rect[3] = height;
		constants.round[0] = corner_radius;
		update_constants(constants);

		ID3D11RenderTargetView* back_rtv[1] = { renderer::g_state.m_p_render_target };
		if (back_rtv[0] == nullptr) return;

		context->OMSetRenderTargets(1, back_rtv, nullptr);
		viewport.Width = static_cast<float>(g_blur.m_width);
		viewport.Height = static_cast<float>(g_blur.m_height);
		context->RSSetViewports(1, &viewport);

		context->OMSetBlendState(g_blur.m_p_blend, nullptr, 0xFFFFFFFF);

		ID3D11ShaderResourceView* temp_srv[1] = { g_blur.m_temp.m_p_srv };
		context->PSSetShaderResources(0, 1, temp_srv);
		context->PSSetShader(g_blur.m_p_composite, nullptr, 0);
		draw_fullscreen();

		context->PSSetShaderResources(0, 1, null_srv);
	}
}
