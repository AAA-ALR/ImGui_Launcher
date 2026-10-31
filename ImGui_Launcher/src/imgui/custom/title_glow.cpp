#include "pch.hpp"

#include "imgui/custom/title_glow.hpp"

#include "renderer_dx11.hpp"
#include "shaders/title_glow_shaders.hpp"

#include <d3dcompiler.h>

// 标题发光：把 ImGui 交给我们的标题顶点重新画进一张 R8 遮罩，
// 做两次可分离高斯，再加法混合回 back buffer。见 title_glow.hpp 的说明。
//
// 中间目标按 band 尺寸建，不是整窗。band 是又扁又宽的矩形，如果映射到窗口
// 比例的目标上，x / y 每 texel 对应的像素数会差好几倍，高斯变成各向异性，
// 光晕被横向拉长。按 band 尺寸建，两个方向都是 1 texel = 1 像素，也只处理
// 标题周围那一条，带宽省下来。band 只在切页时变一次，尺寸变了就重建目标。

namespace title_glow
{
	namespace
	{
		// 高斯每 tap 的步长（像素）。9 tap 覆盖 ±10 像素，两次叠加后约 20px。
		constexpr float k_blur_step = 2.5f;

		// band 边缘羽化宽度（像素），只影响合成，不参与模糊
		constexpr float k_band_feather = 12.0f;

		// 光晕颜色（主题紫）
		constexpr float k_tint_r = 0.62f;
		constexpr float k_tint_g = 0.42f;
		constexpr float k_tint_b = 1.0f;

		// 提交的顶点 / 索引上限。标题最多几十个字形，4096 已远超需要。
		constexpr int k_max_vertices = 4096;
		constexpr int k_max_indices = 8192;

		struct alignas(16) glow_constants
		{
			float band[4];     // xy = band 左上（像素）, zw = band 尺寸
			float texel[4];    // xy = 整窗像素尺寸, zw = 1 / 尺寸
			float step[4];     // xy = uv 步长, z = 强度倍率
			float tint[4];     // rgb = 光晕颜色
			float feather[4];  // x = 羽化宽度（像素）
		};

		static_assert(sizeof(glow_constants) == 80, "title_glow cbuffer layout mismatch");

		struct glow_target
		{
			ID3D11Texture2D* m_p_texture = nullptr;
			ID3D11RenderTargetView* m_p_rtv = nullptr;
			ID3D11ShaderResourceView* m_p_srv = nullptr;
			int m_width = 0;
			int m_height = 0;
		};

		struct glow_runtime
		{
			ID3D11Device* m_p_device = nullptr;
			ID3D11DeviceContext* m_p_context = nullptr;

			ID3D11VertexShader* m_p_mask_vertex = nullptr;
			ID3D11InputLayout* m_p_mask_layout = nullptr;
			ID3D11PixelShader* m_p_mask = nullptr;

			ID3D11VertexShader* m_p_vertex = nullptr;
			ID3D11PixelShader* m_p_blur = nullptr;
			ID3D11PixelShader* m_p_composite = nullptr;

			ID3D11Buffer* m_p_constants = nullptr;
			ID3D11Buffer* m_p_vertex_buffer = nullptr;
			ID3D11Buffer* m_p_index_buffer = nullptr;

			ID3D11SamplerState* m_p_sampler = nullptr;
			ID3D11RasterizerState* m_p_raster = nullptr;
			ID3D11DepthStencilState* m_p_depth = nullptr;
			ID3D11BlendState* m_p_additive = nullptr;

			// 遮罩：字形覆盖率；blur_a / blur_b 是两次高斯的乒乓目标
			glow_target m_mask;
			glow_target m_blur_a;
			glow_target m_blur_b;

			// 这一帧的提交内容
			ImDrawVert m_vertices[k_max_vertices] = {};
			ImDrawIdx m_indices[k_max_indices] = {};
			int m_vertex_count = 0;
			int m_index_count = 0;
			ImVec4 m_band = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

			int m_width = 0;
			int m_height = 0;

			float m_intensity = 1.0f;
			float m_tint[3] = { k_tint_r, k_tint_g, k_tint_b };
			bool m_ready = false;
		};

		glow_runtime g_glow{};

		void release_target(glow_target& target)
		{
			if (target.m_p_srv) { target.m_p_srv->Release(); target.m_p_srv = nullptr; }
			if (target.m_p_rtv) { target.m_p_rtv->Release(); target.m_p_rtv = nullptr; }
			if (target.m_p_texture) { target.m_p_texture->Release(); target.m_p_texture = nullptr; }
			target.m_width = 0;
			target.m_height = 0;
		}

		// R8 目标：只需要 alpha 覆盖率，8 位够用，采样时 UNORM 自动归一化。
		bool create_mask_target(glow_target& target, ID3D11Device* device, int width, int height)
		{
			release_target(target);
			if (width <= 0 || height <= 0) return false;

			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = static_cast<UINT>(width);
			desc.Height = static_cast<UINT>(height);
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8_UNORM;
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

			std::cout << "[title_glow] shader compile failed: " << entry << "\n"
				<< static_cast<const char*>(error_blob->GetBufferPointer()) << '\n';
			error_blob->Release();
		}

		bool compile_vertex(const char* entry, ID3D11VertexShader** out_shader, ID3DBlob** out_blob)
		{
			ID3DBlob* error_blob = nullptr;
			const HRESULT hr = D3DCompile(shader_source::k_title_glow, strlen(shader_source::k_title_glow),
				nullptr, nullptr, nullptr, entry, "vs_5_0", compile_flags(), 0, out_blob, &error_blob);

			if (FAILED(hr))
			{
				report_failure(entry, error_blob);
				return false;
			}

			if (error_blob != nullptr) error_blob->Release();

			const HRESULT create_hr = g_glow.m_p_device->CreateVertexShader(
				(*out_blob)->GetBufferPointer(), (*out_blob)->GetBufferSize(), nullptr, out_shader);
			return SUCCEEDED(create_hr);
		}

		bool compile_pixel(const char* entry, ID3D11PixelShader** out_shader)
		{
			ID3DBlob* shader_blob = nullptr;
			ID3DBlob* error_blob = nullptr;
			const HRESULT hr = D3DCompile(shader_source::k_title_glow, strlen(shader_source::k_title_glow),
				nullptr, nullptr, nullptr, entry, "ps_5_0", compile_flags(), 0, &shader_blob, &error_blob);

			if (FAILED(hr))
			{
				report_failure(entry, error_blob);
				return false;
			}

			if (error_blob != nullptr) error_blob->Release();

			const HRESULT create_hr = g_glow.m_p_device->CreatePixelShader(
				shader_blob->GetBufferPointer(), shader_blob->GetBufferSize(), nullptr, out_shader);
			shader_blob->Release();
			return SUCCEEDED(create_hr);
		}

		void update_constants(const glow_constants& data)
		{
			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (SUCCEEDED(g_glow.m_p_context->Map(g_glow.m_p_constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				memcpy(mapped.pData, &data, sizeof(data));
				g_glow.m_p_context->Unmap(g_glow.m_p_constants, 0);
			}
		}

		void draw_fullscreen()
		{
			g_glow.m_p_context->Draw(3, 0);
		}
	}

	bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
	{
		if (device == nullptr || context == nullptr || width <= 0 || height <= 0) return false;
		if (g_glow.m_ready) return true;

		g_glow.m_p_device = device;
		g_glow.m_p_context = context;
		g_glow.m_width = width;
		g_glow.m_height = height;

		// ---- 顶点着色器：全屏三角形（blur / composite 共用）----
		{
			ID3DBlob* blob = nullptr;
			if (!compile_vertex("vs_fullscreen", &g_glow.m_p_vertex, &blob)) return false;
			blob->Release();
		}

		// ---- 遮罩顶点着色器：ImDrawVert 屏幕坐标 → band 局部裁剪空间 ----
		{
			ID3DBlob* blob = nullptr;
			if (!compile_vertex("vs_mask", &g_glow.m_p_mask_vertex, &blob))
			{
				if (blob != nullptr) blob->Release();
				return false;
			}

			const D3D11_INPUT_ELEMENT_DESC layout[] =
			{
				{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, static_cast<UINT>(offsetof(ImDrawVert, pos)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, static_cast<UINT>(offsetof(ImDrawVert, uv)),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, static_cast<UINT>(offsetof(ImDrawVert, col)), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};

			const HRESULT hr = device->CreateInputLayout(layout, 3,
				blob->GetBufferPointer(), blob->GetBufferSize(), &g_glow.m_p_mask_layout);
			blob->Release();
			if (FAILED(hr)) return false;
		}

		if (!compile_pixel("ps_mask", &g_glow.m_p_mask)) return false;
		if (!compile_pixel("ps_blur", &g_glow.m_p_blur)) return false;
		if (!compile_pixel("ps_composite", &g_glow.m_p_composite)) return false;

		D3D11_BUFFER_DESC buffer_desc = {};
		buffer_desc.ByteWidth = sizeof(glow_constants);
		buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
		buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &g_glow.m_p_constants))) return false;

		buffer_desc.ByteWidth = sizeof(ImDrawVert) * k_max_vertices;
		buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &g_glow.m_p_vertex_buffer))) return false;

		buffer_desc.ByteWidth = sizeof(ImDrawIdx) * k_max_indices;
		buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, &g_glow.m_p_index_buffer))) return false;

		D3D11_SAMPLER_DESC sampler_desc = {};
		sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler_desc.MinLOD = 0.0f;
		sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampler_desc, &g_glow.m_p_sampler))) return false;

		D3D11_RASTERIZER_DESC raster_desc = {};
		raster_desc.FillMode = D3D11_FILL_SOLID;
		raster_desc.CullMode = D3D11_CULL_NONE;
		raster_desc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&raster_desc, &g_glow.m_p_raster))) return false;

		D3D11_DEPTH_STENCIL_DESC depth_desc = {};
		depth_desc.DepthEnable = FALSE;
		depth_desc.StencilEnable = FALSE;
		if (FAILED(device->CreateDepthStencilState(&depth_desc, &g_glow.m_p_depth))) return false;

		// 加法混合：目标像素只会被加亮
		{
			D3D11_BLEND_DESC blend_desc = {};
			blend_desc.RenderTarget[0].BlendEnable = TRUE;
			blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
			blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(device->CreateBlendState(&blend_desc, &g_glow.m_p_additive))) return false;
		}

		// 目标按 band 建，第一帧提交之前还不知道尺寸，先用窗口尺寸占位
		if (!create_mask_target(g_glow.m_mask, device, width, height)) return false;
		if (!create_mask_target(g_glow.m_blur_a, device, width, height)) return false;
		if (!create_mask_target(g_glow.m_blur_b, device, width, height)) return false;

		g_glow.m_ready = true;
		return true;
	}

	void shutdown()
	{
		if (!g_glow.m_ready && g_glow.m_p_device == nullptr) return;

		release_target(g_glow.m_mask);
		release_target(g_glow.m_blur_a);
		release_target(g_glow.m_blur_b);

		if (g_glow.m_p_mask) { g_glow.m_p_mask->Release(); g_glow.m_p_mask = nullptr; }
		if (g_glow.m_p_blur) { g_glow.m_p_blur->Release(); g_glow.m_p_blur = nullptr; }
		if (g_glow.m_p_composite) { g_glow.m_p_composite->Release(); g_glow.m_p_composite = nullptr; }
		if (g_glow.m_p_mask_vertex) { g_glow.m_p_mask_vertex->Release(); g_glow.m_p_mask_vertex = nullptr; }
		if (g_glow.m_p_mask_layout) { g_glow.m_p_mask_layout->Release(); g_glow.m_p_mask_layout = nullptr; }
		if (g_glow.m_p_vertex) { g_glow.m_p_vertex->Release(); g_glow.m_p_vertex = nullptr; }
		if (g_glow.m_p_constants) { g_glow.m_p_constants->Release(); g_glow.m_p_constants = nullptr; }
		if (g_glow.m_p_vertex_buffer) { g_glow.m_p_vertex_buffer->Release(); g_glow.m_p_vertex_buffer = nullptr; }
		if (g_glow.m_p_index_buffer) { g_glow.m_p_index_buffer->Release(); g_glow.m_p_index_buffer = nullptr; }
		if (g_glow.m_p_sampler) { g_glow.m_p_sampler->Release(); g_glow.m_p_sampler = nullptr; }
		if (g_glow.m_p_raster) { g_glow.m_p_raster->Release(); g_glow.m_p_raster = nullptr; }
		if (g_glow.m_p_depth) { g_glow.m_p_depth->Release(); g_glow.m_p_depth = nullptr; }
		if (g_glow.m_p_additive) { g_glow.m_p_additive->Release(); g_glow.m_p_additive = nullptr; }

		g_glow.m_p_device = nullptr;
		g_glow.m_p_context = nullptr;
		g_glow.m_ready = false;
	}

	void set_intensity(float intensity)
	{
		g_glow.m_intensity = std::clamp(intensity, 0.0f, 4.0f);
	}

	void set_tint(float red, float green, float blue)
	{
		g_glow.m_tint[0] = red;
		g_glow.m_tint[1] = green;
		g_glow.m_tint[2] = blue;
	}

	void submit(const ImDrawVert* vertices, int vertex_begin, int vertex_end,
		const ImDrawIdx* indices, int index_begin, int index_end,
		const ImVec4& band)
	{
		if (!g_glow.m_ready) return;
		if (vertices == nullptr || indices == nullptr) return;
		if (vertex_end <= vertex_begin || index_end <= index_begin) return;

		const int vertex_count = std::min(vertex_end - vertex_begin, k_max_vertices);
		const int index_count = std::min(index_end - index_begin, k_max_indices);

		memcpy(g_glow.m_vertices, vertices + vertex_begin, sizeof(ImDrawVert) * vertex_count);

		// 索引是相对整条 DrawList 的顶点序号，这里整体平移回 0 起点
		for (int i_index = 0; i_index < index_count; ++i_index)
		{
			const ImDrawIdx source = indices[index_begin + i_index];
			const int local = static_cast<int>(source) - vertex_begin;
			g_glow.m_indices[i_index] = static_cast<ImDrawIdx>(std::clamp(local, 0, vertex_count - 1));
		}

		g_glow.m_vertex_count = vertex_count;
		g_glow.m_index_count = index_count;
		g_glow.m_band = band;
	}

	void render(ID3D11ShaderResourceView* p_atlas)
	{
		if (!g_glow.m_ready || p_atlas == nullptr) return;
		if (g_glow.m_vertex_count <= 0 || g_glow.m_index_count <= 0) return;

		const ImVec4& band = g_glow.m_band;
		if (band.z <= 0.0f || band.w <= 0.0f)
		{
			g_glow.m_vertex_count = 0;
			g_glow.m_index_count = 0;
			return;
		}

		// 目标尺寸跟着 band 走；band 只在切页时变，尺寸不同才重建
		const int target_width = std::max(1, static_cast<int>(band.z));
		const int target_height = std::max(1, static_cast<int>(band.w));

		if (g_glow.m_mask.m_width != target_width || g_glow.m_mask.m_height != target_height)
		{
			if (!create_mask_target(g_glow.m_mask, g_glow.m_p_device, target_width, target_height)) return;
			if (!create_mask_target(g_glow.m_blur_a, g_glow.m_p_device, target_width, target_height)) return;
			if (!create_mask_target(g_glow.m_blur_b, g_glow.m_p_device, target_width, target_height)) return;
		}

		// 每帧重建目标太贵：band 尺寸只跟「标题文字宽度」有关，
		// 而面板宽度固定，所以同一次运行里它其实是常量。
		// 真正会变的是加载页（全屏，没有 band），那一帧我们直接跳过。

		const float band_width = static_cast<float>(target_width);
		const float band_height = static_cast<float>(target_height);

		ID3D11DeviceContext* context = g_glow.m_p_context;

		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->RSSetState(g_glow.m_p_raster);
		context->OMSetDepthStencilState(g_glow.m_p_depth, 0);

		ID3D11SamplerState* samplers[1] = { g_glow.m_p_sampler };
		context->PSSetSamplers(0, 1, samplers);

		ID3D11Buffer* ps_buffers[1] = { g_glow.m_p_constants };
		context->PSSetConstantBuffers(0, 1, ps_buffers);

		// 遮罩与模糊都画到 band 目标上，viewport 就是目标尺寸
		D3D11_VIEWPORT band_viewport = {};
		band_viewport.MinDepth = 0.0f;
		band_viewport.MaxDepth = 1.0f;
		band_viewport.TopLeftX = 0.0f;
		band_viewport.TopLeftY = 0.0f;
		band_viewport.Width = band_width;
		band_viewport.Height = band_height;
		context->RSSetViewports(1, &band_viewport);

		glow_constants constants = {};
		constants.band[0] = band.x;
		constants.band[1] = band.y;
		constants.band[2] = band.z;
		constants.band[3] = band.w;
		constants.texel[0] = static_cast<float>(g_glow.m_width);
		constants.texel[1] = static_cast<float>(g_glow.m_height);
		constants.texel[2] = 1.0f / static_cast<float>(g_glow.m_width);
		constants.texel[3] = 1.0f / static_cast<float>(g_glow.m_height);
		constants.tint[0] = g_glow.m_tint[0];
		constants.tint[1] = g_glow.m_tint[1];
		constants.tint[2] = g_glow.m_tint[2];
		constants.tint[3] = 1.0f;
		constants.feather[0] = k_band_feather;
		constants.step[2] = g_glow.m_intensity;
		update_constants(constants);

		ID3D11ShaderResourceView* null_srv[1] = { nullptr };
		ID3D11RenderTargetView* rtv[1] = { nullptr };

		// ---- 1. 遮罩：标题顶点 → R8 ----
		{
			const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearRenderTargetView(g_glow.m_mask.m_p_rtv, clear);

			rtv[0] = g_glow.m_mask.m_p_rtv;
			context->OMSetRenderTargets(1, rtv, nullptr);

			// 遮罩阶段不混合，清成 0 之后画上去的就是覆盖率
			context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

			D3D11_MAPPED_SUBRESOURCE mapped = {};
			if (SUCCEEDED(context->Map(g_glow.m_p_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				memcpy(mapped.pData, g_glow.m_vertices, sizeof(ImDrawVert) * g_glow.m_vertex_count);
				context->Unmap(g_glow.m_p_vertex_buffer, 0);
			}

			if (SUCCEEDED(context->Map(g_glow.m_p_index_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			{
				memcpy(mapped.pData, g_glow.m_indices, sizeof(ImDrawIdx) * g_glow.m_index_count);
				context->Unmap(g_glow.m_p_index_buffer, 0);
			}

			const UINT stride = sizeof(ImDrawVert);
			const UINT offset = 0;
			ID3D11Buffer* vertex_buffers[1] = { g_glow.m_p_vertex_buffer };
			context->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
			context->IASetIndexBuffer(g_glow.m_p_index_buffer,
				sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
			context->IASetInputLayout(g_glow.m_p_mask_layout);
			context->VSSetShader(g_glow.m_p_mask_vertex, nullptr, 0);

			ID3D11Buffer* vs_buffers[1] = { g_glow.m_p_constants };
			context->VSSetConstantBuffers(0, 1, vs_buffers);
			context->PSSetShader(g_glow.m_p_mask, nullptr, 0);

			ID3D11ShaderResourceView* atlas[1] = { p_atlas };
			context->PSSetShaderResources(0, 1, atlas);

			context->DrawIndexed(static_cast<UINT>(g_glow.m_index_count), 0, 0);
		}

		context->PSSetShaderResources(0, 1, null_srv);
		context->IASetInputLayout(nullptr);

		// 之后都是全屏三角形
		context->VSSetShader(g_glow.m_p_vertex, nullptr, 0);

		// ---- 2. 水平高斯：mask -> blur_a ----
		// 目标是 band 尺寸，1 texel = 1 像素，步长 = 每 tap 像素数 / band 宽
		constants.step[0] = k_blur_step / band_width;
		constants.step[1] = 0.0f;
		update_constants(constants);

		rtv[0] = g_glow.m_blur_a.m_p_rtv;
		context->OMSetRenderTargets(1, rtv, nullptr);
		ID3D11ShaderResourceView* mask_srv[1] = { g_glow.m_mask.m_p_srv };
		context->PSSetShaderResources(0, 1, mask_srv);
		context->PSSetShader(g_glow.m_p_blur, nullptr, 0);
		draw_fullscreen();

		context->PSSetShaderResources(0, 1, null_srv);

		// ---- 3. 垂直高斯：blur_a -> blur_b ----
		constants.step[0] = 0.0f;
		constants.step[1] = k_blur_step / band_height;
		update_constants(constants);

		rtv[0] = g_glow.m_blur_b.m_p_rtv;
		context->OMSetRenderTargets(1, rtv, nullptr);
		ID3D11ShaderResourceView* a_srv[1] = { g_glow.m_blur_a.m_p_srv };
		context->PSSetShaderResources(0, 1, a_srv);
		context->PSSetShader(g_glow.m_p_blur, nullptr, 0);
		draw_fullscreen();

		context->PSSetShaderResources(0, 1, null_srv);

		// ---- 4. 合成回 back buffer ----
		// 画到整窗：uv 与屏幕像素一一对应，shader 里换算回 band 局部坐标，
		// band 之外直接输出 0，所以只有标题周围那一圈会亮。
		ID3D11RenderTargetView* back_rtv[1] = { renderer::g_state.m_p_render_target };
		if (back_rtv[0] == nullptr) return;
		context->OMSetRenderTargets(1, back_rtv, nullptr);

		D3D11_VIEWPORT window_viewport = band_viewport;
		window_viewport.Width = static_cast<float>(g_glow.m_width);
		window_viewport.Height = static_cast<float>(g_glow.m_height);
		context->RSSetViewports(1, &window_viewport);

		context->OMSetBlendState(g_glow.m_p_additive, nullptr, 0xFFFFFFFF);

		ID3D11ShaderResourceView* b_srv[1] = { g_glow.m_blur_b.m_p_srv };
		context->PSSetShaderResources(0, 1, b_srv);
		context->PSSetShader(g_glow.m_p_composite, nullptr, 0);
		draw_fullscreen();

		context->PSSetShaderResources(0, 1, null_srv);
		context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

		g_glow.m_vertex_count = 0;
		g_glow.m_index_count = 0;
	}
}
