#include "pch.hpp"

#include "effects/burst_background.hpp"
#include "renderer_dx11.hpp"
#include "shaders/hlsl_source.hpp"

#include <d3dcompiler.h>

namespace burst_background
{
	// 与 HLSL cbuffer 严格对应，四个 float4 一段，不能有 padding 差异
	struct burst_constants
	{
		float res_time[4];    // res.xy, time, phase
		float intensity[4];   // intensity, distort, grain, zoom
		float shape[4];       // ray_width, twist, flow, center_fade
		float counts[4];      // ray_count, color_count, encode, -
		float offset[4];      // center offset, px
		float pointer[4];     // pointer pos, strength, radius
		float intro[4];       // reach, strength, active
		float pulses[4][4];   // radius, strength, width, lift
		float orient0[4];
		float orient1[4];
		float orient2[4];
	};

	struct downsample_constants
	{
		float target[4];
		float texel[4];
		float limit[4];
	};

	struct blur_constants
	{
		float target[4];
		float step[4];
	};

	struct composite_constants
	{
		float scene_size[4];
		float view[4];
		float decode[4];
	};

	static_assert(sizeof(burst_constants) == 224, "burst cbuffer layout mismatch");
	static_assert(sizeof(downsample_constants) == 48, "downsample cbuffer layout mismatch");
	static_assert(sizeof(blur_constants) == 32, "blur cbuffer layout mismatch");
	static_assert(sizeof(composite_constants) == 48, "composite cbuffer layout mismatch");

	namespace
	{
		constexpr int k_bloom_levels = 4;
		constexpr int k_max_pulses = 4;
		constexpr float k_pulse_life = 1.5f;
		constexpr float k_intro_seconds = 2.4f;

		struct alignas(16) gpu_target
		{
			ID3D11Texture2D* m_p_texture = nullptr;
			ID3D11RenderTargetView* m_p_rtv = nullptr;
			ID3D11ShaderResourceView* m_p_srv = nullptr;
			int m_width = 0;
			int m_height = 0;
		};

		struct pulse
		{
			float age = 0.0f;
		};

		// 运行状态
		struct runtime
		{
			ID3D11Device* m_p_device = nullptr;
			ID3D11DeviceContext* m_p_context = nullptr;

			ID3D11VertexShader* m_p_vertex = nullptr;
			ID3D11PixelShader* m_p_burst = nullptr;
			ID3D11PixelShader* m_p_downsample = nullptr;
			ID3D11PixelShader* m_p_blur = nullptr;
			ID3D11PixelShader* m_p_composite = nullptr;

			ID3D11Buffer* m_p_burst_cb = nullptr;
			ID3D11Buffer* m_p_downsample_cb = nullptr;
			ID3D11Buffer* m_p_blur_cb = nullptr;
			ID3D11Buffer* m_p_composite_cb = nullptr;

			ID3D11SamplerState* m_p_sampler = nullptr;
			ID3D11RasterizerState* m_p_raster = nullptr;
			ID3D11BlendState* m_p_blend = nullptr;
			ID3D11DepthStencilState* m_p_depth = nullptr;

			ID3D11Texture2D* m_p_gradient = nullptr;
			ID3D11ShaderResourceView* m_p_gradient_srv = nullptr;

			gpu_target m_scene;
			gpu_target m_bloom_front[k_bloom_levels];
			gpu_target m_bloom_back[k_bloom_levels];

			int m_width = 0;
			int m_height = 0;
			int m_scene_width = 0;
			int m_scene_height = 0;

			float m_time = 0.0f;
			float m_phase = 0.0f;
			float m_flow = 0.0f;
			float m_kick = 0.0f;
			float m_intro = 0.0f;

			float m_pointer_x = 0.0f;
			float m_pointer_y = 0.0f;
			bool m_pointer_inside = false;

			pulse m_pulses[k_max_pulses];
			int m_pulse_count = 0;

			bool m_ready = false;
		};

		runtime g_runtime{};
		settings g_settings{};
	}

	settings& config()
	{
		return g_settings;
	}

	// ---------------------------------------------------------------- 着色器编译

	static bool compile_shader(const char* source, const char* entry, const char* target,
		ID3DBlob** out_blob)
	{
		UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR;
#ifdef _DEBUG
		flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

		ID3DBlob* error_blob = nullptr;
		const HRESULT hr = D3DCompile(
			source, strlen(source), nullptr, nullptr, nullptr,
			entry, target, flags, 0, out_blob, &error_blob);

		if (FAILED(hr))
		{
			if (error_blob != nullptr)
			{
				std::cout << u8"[burst] 着色器编译失败 " << entry << ":\n"
					<< static_cast<const char*>(error_blob->GetBufferPointer()) << '\n';
				error_blob->Release();
			}
			return false;
		}

		if (error_blob != nullptr) error_blob->Release();
		return true;
	}

	template <typename T_shader>
	static bool create_shader(const char* source, const char* entry, const char* target,
		ID3D11Device* device, T_shader** out_shader)
	{
		ID3DBlob* blob = nullptr;
		if (!compile_shader(source, entry, target, &blob)) return false;

		const HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_shader);
		blob->Release();
		return SUCCEEDED(hr);
	}

	static bool create_vertex_shader(const char* source, ID3D11Device* device, ID3D11VertexShader** out_shader)
	{
		ID3DBlob* blob = nullptr;
		if (!compile_shader(source, "vs_main", "vs_5_0", &blob)) return false;

		const HRESULT hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_shader);
		blob->Release();
		return SUCCEEDED(hr);
	}

	static bool create_constant_buffer(ID3D11Device* device, size_t size, ID3D11Buffer** out_buffer)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.ByteWidth = static_cast<UINT>(size);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		return SUCCEEDED(device->CreateBuffer(&desc, nullptr, out_buffer));
	}

	// ---------------------------------------------------------------- 渲染目标

	static void release_target(gpu_target& target)
	{
		if (target.m_p_srv) { target.m_p_srv->Release(); target.m_p_srv = nullptr; }
		if (target.m_p_rtv) { target.m_p_rtv->Release(); target.m_p_rtv = nullptr; }
		if (target.m_p_texture) { target.m_p_texture->Release(); target.m_p_texture = nullptr; }
		target.m_width = 0;
		target.m_height = 0;
	}

	static bool create_target(gpu_target& target, ID3D11Device* device, int width, int height)
	{
		if (target.m_p_texture != nullptr && target.m_width == width && target.m_height == height)
			return true;

		release_target(target);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = static_cast<UINT>(width);
		desc.Height = static_cast<UINT>(height);
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		if (FAILED(device->CreateTexture2D(&desc, nullptr, &target.m_p_texture)))
			return false;

		if (FAILED(device->CreateRenderTargetView(target.m_p_texture, nullptr, &target.m_p_rtv)))
			return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		if (FAILED(device->CreateShaderResourceView(target.m_p_texture, &srv_desc, &target.m_p_srv)))
			return false;

		target.m_width = width;
		target.m_height = height;
		return true;
	}

	// ---------------------------------------------------------------- 渐变纹理

	static void release_gradient()
	{
		if (g_runtime.m_p_gradient_srv) { g_runtime.m_p_gradient_srv->Release(); g_runtime.m_p_gradient_srv = nullptr; }
		if (g_runtime.m_p_gradient) { g_runtime.m_p_gradient->Release(); g_runtime.m_p_gradient = nullptr; }
	}

	static bool upload_gradient()
	{
		const size_t count = g_settings.colors.empty() ? 1u : g_settings.colors.size();

		std::vector<std::array<float, 4>> texels(count);
		for (size_t index = 0; index < g_settings.colors.size(); ++index)
		{
			const std::array<float, 3>& color = g_settings.colors[index];
			texels[index] = { color[0], color[1], color[2], 1.0f };
		}
		if (g_settings.colors.empty())
			texels[0] = { 1.0f, 1.0f, 1.0f, 1.0f };

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = static_cast<UINT>(count);
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA data = {};
		data.pSysMem = texels.data();
		data.SysMemPitch = static_cast<UINT>(count * sizeof(float) * 4);

		ID3D11Texture2D* texture = nullptr;
		if (FAILED(g_runtime.m_p_device->CreateTexture2D(&desc, &data, &texture)))
			return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		const HRESULT hr = g_runtime.m_p_device->CreateShaderResourceView(texture, &srv_desc, &g_runtime.m_p_gradient_srv);
		if (g_runtime.m_p_gradient) g_runtime.m_p_gradient->Release();
		g_runtime.m_p_gradient = texture;

		return SUCCEEDED(hr);
	}

	static bool upload_gradient_if_needed()
	{
		static std::vector<std::array<float, 3>> s_last_colors;
		static bool s_uploaded = false;

		if (s_uploaded && s_last_colors == g_settings.colors)
			return true;

		s_last_colors = g_settings.colors;
		s_uploaded = true;
		return upload_gradient();
	}

	// ---------------------------------------------------------------- 生命周期

	bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
	{
		if (device == nullptr || context == nullptr || width <= 0 || height <= 0)
			return false;

		g_runtime.m_p_device = device;
		g_runtime.m_p_context = context;
		g_runtime.m_width = width;
		g_runtime.m_height = height;

		const float scale = std::clamp(g_settings.resolution_scale, 0.2f, 1.0f);
		g_runtime.m_scene_width = std::max(1, static_cast<int>(width * scale));
		g_runtime.m_scene_height = std::max(1, static_cast<int>(height * scale));

		if (!create_vertex_shader(shader_source::k_fullscreen, device, &g_runtime.m_p_vertex)) return false;
		if (!create_shader(shader_source::k_burst, "ps_main", "ps_5_0", device, &g_runtime.m_p_burst)) return false;
		if (!create_shader(shader_source::k_downsample, "ps_main", "ps_5_0", device, &g_runtime.m_p_downsample)) return false;
		if (!create_shader(shader_source::k_blur, "ps_main", "ps_5_0", device, &g_runtime.m_p_blur)) return false;
		if (!create_shader(shader_source::k_composite, "ps_main", "ps_5_0", device, &g_runtime.m_p_composite)) return false;

		if (!create_constant_buffer(device, sizeof(burst_constants), &g_runtime.m_p_burst_cb)) return false;
		if (!create_constant_buffer(device, sizeof(downsample_constants), &g_runtime.m_p_downsample_cb)) return false;
		if (!create_constant_buffer(device, sizeof(blur_constants), &g_runtime.m_p_blur_cb)) return false;
		if (!create_constant_buffer(device, sizeof(composite_constants), &g_runtime.m_p_composite_cb)) return false;

		D3D11_SAMPLER_DESC sampler_desc = {};
		sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sampler_desc.MinLOD = 0.0f;
		sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampler_desc, &g_runtime.m_p_sampler))) return false;

		D3D11_RASTERIZER_DESC raster_desc = {};
		raster_desc.FillMode = D3D11_FILL_SOLID;
		raster_desc.CullMode = D3D11_CULL_NONE;
		raster_desc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&raster_desc, &g_runtime.m_p_raster))) return false;

		D3D11_BLEND_DESC blend_desc = {};
		blend_desc.RenderTarget[0].BlendEnable = FALSE;
		blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blend_desc, &g_runtime.m_p_blend))) return false;

		D3D11_DEPTH_STENCIL_DESC depth_desc = {};
		depth_desc.DepthEnable = FALSE;
		depth_desc.StencilEnable = FALSE;
		if (FAILED(device->CreateDepthStencilState(&depth_desc, &g_runtime.m_p_depth))) return false;

		if (!create_target(g_runtime.m_scene, device, g_runtime.m_scene_width, g_runtime.m_scene_height)) return false;

		// bloom 每一级都按输出分辨率的一半递减，两张交替做水平 / 垂直模糊
		for (int level = 0; level < k_bloom_levels; ++level)
		{
			const int level_width = std::max(1, width >> (level + 1));
			const int level_height = std::max(1, height >> (level + 1));

			if (!create_target(g_runtime.m_bloom_front[level], device, level_width, level_height)) return false;
			if (!create_target(g_runtime.m_bloom_back[level], device, level_width, level_height)) return false;
		}

		if (!upload_gradient()) return false;

		g_runtime.m_intro = g_settings.intro ? 0.0f : 1.0f;
		g_runtime.m_ready = true;
		return true;
	}

	void shutdown()
	{
		if (!g_runtime.m_ready && g_runtime.m_p_device == nullptr) return;

		release_target(g_runtime.m_scene);
		for (int level = 0; level < k_bloom_levels; ++level)
		{
			release_target(g_runtime.m_bloom_front[level]);
			release_target(g_runtime.m_bloom_back[level]);
		}

		release_gradient();

		if (g_runtime.m_p_vertex) { g_runtime.m_p_vertex->Release(); g_runtime.m_p_vertex = nullptr; }
		if (g_runtime.m_p_burst) { g_runtime.m_p_burst->Release(); g_runtime.m_p_burst = nullptr; }
		if (g_runtime.m_p_downsample) { g_runtime.m_p_downsample->Release(); g_runtime.m_p_downsample = nullptr; }
		if (g_runtime.m_p_blur) { g_runtime.m_p_blur->Release(); g_runtime.m_p_blur = nullptr; }
		if (g_runtime.m_p_composite) { g_runtime.m_p_composite->Release(); g_runtime.m_p_composite = nullptr; }

		if (g_runtime.m_p_burst_cb) { g_runtime.m_p_burst_cb->Release(); g_runtime.m_p_burst_cb = nullptr; }
		if (g_runtime.m_p_downsample_cb) { g_runtime.m_p_downsample_cb->Release(); g_runtime.m_p_downsample_cb = nullptr; }
		if (g_runtime.m_p_blur_cb) { g_runtime.m_p_blur_cb->Release(); g_runtime.m_p_blur_cb = nullptr; }
		if (g_runtime.m_p_composite_cb) { g_runtime.m_p_composite_cb->Release(); g_runtime.m_p_composite_cb = nullptr; }

		if (g_runtime.m_p_sampler) { g_runtime.m_p_sampler->Release(); g_runtime.m_p_sampler = nullptr; }
		if (g_runtime.m_p_raster) { g_runtime.m_p_raster->Release(); g_runtime.m_p_raster = nullptr; }
		if (g_runtime.m_p_blend) { g_runtime.m_p_blend->Release(); g_runtime.m_p_blend = nullptr; }
		if (g_runtime.m_p_depth) { g_runtime.m_p_depth->Release(); g_runtime.m_p_depth = nullptr; }

		g_runtime.m_ready = false;
		g_runtime.m_p_device = nullptr;
		g_runtime.m_p_context = nullptr;
	}

	// ---------------------------------------------------------------- 事件触发

	void trigger()
	{
		if (!g_runtime.m_ready) return;
		if (!g_settings.click_burst) return;

		if (g_runtime.m_pulse_count >= k_max_pulses)
		{
			// 顶掉最老的那一道
			for (int index = 1; index < k_max_pulses; ++index)
				g_runtime.m_pulses[index - 1] = g_runtime.m_pulses[index];

			g_runtime.m_pulse_count = k_max_pulses - 1;
		}

		g_runtime.m_pulses[g_runtime.m_pulse_count].age = 0.0f;
		++g_runtime.m_pulse_count;

		g_runtime.m_kick = std::min(5.0f, g_runtime.m_kick + 2.4f);
	}

	void reset()
	{
		g_runtime.m_pulse_count = 0;
		g_runtime.m_kick = 0.0f;
		g_runtime.m_time = 0.0f;
		g_runtime.m_phase = 0.0f;
		g_runtime.m_flow = 0.0f;
		g_runtime.m_intro = g_settings.intro ? 0.0f : 1.0f;
	}

	bool is_bursting()
	{
		return g_runtime.m_pulse_count > 0;
	}

	void set_pointer(float x, float y, bool inside)
	{
		g_runtime.m_pointer_x = x;
		g_runtime.m_pointer_y = y;
		g_runtime.m_pointer_inside = inside;
	}

	// ---------------------------------------------------------------- 矩阵

	static void multiply3(const float a[9], const float b[9], float out[9])
	{
		for (int col = 0; col < 3; ++col)
		{
			for (int row = 0; row < 3; ++row)
			{
				out[col * 3 + row] =
					a[row] * b[col * 3] +
					a[3 + row] * b[col * 3 + 1] +
					a[6 + row] * b[col * 3 + 2];
			}
		}
	}

	static void rotation_x(float angle, float out[9])
	{
		const float c = std::cos(angle);
		const float s = std::sin(angle);
		out[0] = 1; out[1] = 0; out[2] = 0;
		out[3] = 0; out[4] = c; out[5] = -s;
		out[6] = 0; out[7] = s; out[8] = c;
	}

	static void rotation_y(float angle, float out[9])
	{
		const float c = std::cos(angle);
		const float s = std::sin(angle);
		out[0] = c; out[1] = 0; out[2] = s;
		out[3] = 0; out[4] = 1; out[5] = 0;
		out[6] = -s; out[7] = 0; out[8] = c;
	}

	static void rotation_z(float angle, float out[9])
	{
		const float c = std::cos(angle);
		const float s = std::sin(angle);
		out[0] = c; out[1] = -s; out[2] = 0;
		out[3] = s; out[4] = c; out[5] = 0;
		out[6] = 0; out[7] = 0; out[8] = 1;
	}

	// ---------------------------------------------------------------- 绘制辅助

	static void update_constant_buffer(ID3D11Buffer* buffer, const void* data, size_t size)
	{
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (SUCCEEDED(g_runtime.m_p_context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		{
			memcpy(mapped.pData, data, size);
			g_runtime.m_p_context->Unmap(buffer, 0);
		}
	}

	static void draw_fullscreen()
	{
		g_runtime.m_p_context->Draw(3, 0);
	}

	// ---------------------------------------------------------------- 渲染

	void render(float delta_seconds)
	{
		if (!g_runtime.m_ready) return;

		ID3D11DeviceContext* context = g_runtime.m_p_context;
		const float dt = std::clamp(delta_seconds, 0.0f, 0.05f);

		// 时间推进
		if (g_settings.intro)
			g_runtime.m_intro = std::min(1.0f, g_runtime.m_intro + dt / k_intro_seconds);
		else
			g_runtime.m_intro = 1.0f;

		const float rush = 3.0f * std::pow(1.0f - g_runtime.m_intro, 2.0f);
		g_runtime.m_time += dt;
		g_runtime.m_phase += dt * g_settings.speed * (1.0f + rush) + g_runtime.m_kick * dt;
		g_runtime.m_flow += dt * g_settings.flow * 1.2f;

		g_runtime.m_kick *= std::exp(-dt * 3.5f);
		if (g_runtime.m_kick < 1e-3f) g_runtime.m_kick = 0.0f;

		// 光波老化
		for (int index = g_runtime.m_pulse_count - 1; index >= 0; --index)
		{
			g_runtime.m_pulses[index].age += dt;
			if (g_runtime.m_pulses[index].age > k_pulse_life)
			{
				for (int move = index; move < g_runtime.m_pulse_count - 1; ++move)
					g_runtime.m_pulses[move] = g_runtime.m_pulses[move + 1];

				--g_runtime.m_pulse_count;
			}
		}

		if (!upload_gradient_if_needed()) return;

		// 公共管线状态
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_runtime.m_p_vertex, nullptr, 0);
		context->RSSetState(g_runtime.m_p_raster);
		context->OMSetBlendState(g_runtime.m_p_blend, nullptr, 0xFFFFFFFF);
		context->OMSetDepthStencilState(g_runtime.m_p_depth, 0);
		ID3D11SamplerState* samplers[1] = { g_runtime.m_p_sampler };
		context->PSSetSamplers(0, 1, samplers);

		const int scene_w = g_runtime.m_scene_width;
		const int scene_h = g_runtime.m_scene_height;

		// ---- 1. 光线步进 ----
		burst_constants burst_cb = {};
		burst_cb.res_time[0] = static_cast<float>(scene_w);
		burst_cb.res_time[1] = static_cast<float>(scene_h);
		burst_cb.res_time[2] = g_runtime.m_time;
		burst_cb.res_time[3] = g_runtime.m_phase;

		burst_cb.intensity[0] = g_settings.intensity;
		burst_cb.intensity[1] = g_settings.distort;
		burst_cb.intensity[2] = g_settings.grain;
		burst_cb.intensity[3] = g_settings.zoom;

		burst_cb.shape[0] = g_settings.ray_width;
		burst_cb.shape[1] = g_settings.twist;
		burst_cb.shape[2] = g_runtime.m_flow;
		burst_cb.shape[3] = g_settings.center_fade;

		burst_cb.counts[0] = static_cast<float>(g_settings.ray_count);
		burst_cb.counts[1] = static_cast<float>(g_settings.colors.size());
		burst_cb.counts[2] = 1.0f; // Float 目标，不需要 encode 缩放

		const float scene_scale = static_cast<float>(scene_w) / static_cast<float>(g_runtime.m_width);
		burst_cb.offset[0] = 0.0f;
		burst_cb.offset[1] = 0.0f;
		burst_cb.offset[2] = 0.0f;
		burst_cb.offset[3] = 0.0f;
		burst_cb.pointer[0] = g_runtime.m_pointer_x * scene_scale;
		burst_cb.pointer[1] = g_runtime.m_pointer_y * scene_scale;
		burst_cb.pointer[2] = (g_settings.mouse_interaction && g_runtime.m_pointer_inside)
			? 0.7f * g_settings.mouse_strength : 0.0f;
		burst_cb.pointer[3] = 0.45f;

		for (int index = 0; index < k_max_pulses; ++index)
		{
			if (index >= g_runtime.m_pulse_count) continue;

			const float age = g_runtime.m_pulses[index].age;
			const float life = age / k_pulse_life;

			burst_cb.pulses[index][0] = age * 1.7f;
			burst_cb.pulses[index][1] = 1.6f * std::pow(1.0f - life, 1.6f);
			burst_cb.pulses[index][2] = 0.2f + age * 0.16f;
			burst_cb.pulses[index][3] = 0.35f * std::exp(-age * 7.0f);
		}

		const float unit = 0.5f * static_cast<float>(std::min(scene_w, scene_h));
		const float reach =
			std::hypot(scene_w * 0.5f, scene_h * 0.5f) / std::max(unit, 1.0f);
		const float rise = 1.0f - std::pow(1.0f - g_runtime.m_intro, 3.0f);

		burst_cb.intro[0] = (rise * (reach + 0.6f)) / 0.7f;
		burst_cb.intro[1] = 1.6f * (1.0f - g_runtime.m_intro);
		burst_cb.intro[2] = (g_runtime.m_intro < 1.0f) ? 1.0f : 0.0f;

		float orient[9];
		{
			float rz[9], ry[9], rx[9], tmp[9];
			rotation_z(g_runtime.m_phase * 0.17f, rz);
			rotation_y(g_runtime.m_phase * 0.21f, ry);
			rotation_x(g_runtime.m_phase * 0.31f, rx);
			multiply3(rz, ry, tmp);
			multiply3(tmp, rx, orient);
		}

		burst_cb.orient0[0] = orient[0]; burst_cb.orient0[1] = orient[1]; burst_cb.orient0[2] = orient[2];
		burst_cb.orient1[0] = orient[3]; burst_cb.orient1[1] = orient[4]; burst_cb.orient1[2] = orient[5];
		burst_cb.orient2[0] = orient[6]; burst_cb.orient2[1] = orient[7]; burst_cb.orient2[2] = orient[8];

		update_constant_buffer(g_runtime.m_p_burst_cb, &burst_cb, sizeof(burst_cb));

		ID3D11Buffer* burst_cbs[1] = { g_runtime.m_p_burst_cb };
		ID3D11ShaderResourceView* gradient_srvs[1] = { g_runtime.m_p_gradient_srv };

		ID3D11RenderTargetView* scene_rtv[1] = { g_runtime.m_scene.m_p_rtv };
		context->OMSetRenderTargets(1, scene_rtv, nullptr);
		D3D11_VIEWPORT viewport = {};
		viewport.Width = static_cast<float>(scene_w);
		viewport.Height = static_cast<float>(scene_h);
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
		context->PSSetConstantBuffers(0, 1, burst_cbs);
		context->PSSetShaderResources(0, 1, gradient_srvs);
		context->PSSetShader(g_runtime.m_p_burst, nullptr, 0);
		draw_fullscreen();

		// 解绑，避免同一个资源同时作为 SRV 和 RTV
		ID3D11ShaderResourceView* null_srv[1] = { nullptr };
		context->PSSetShaderResources(0, 1, null_srv);

		// ---- 2. Bloom 下采样 + 模糊 ----
		if (g_settings.glow > 0.0f)
		{
			const gpu_target* source = &g_runtime.m_scene;

			for (int level = 0; level < k_bloom_levels; ++level)
			{
				gpu_target& front = g_runtime.m_bloom_front[level];
				gpu_target& back = g_runtime.m_bloom_back[level];

				// 下采样
				downsample_constants down_cb = {};
				down_cb.target[0] = static_cast<float>(front.m_width);
				down_cb.target[1] = static_cast<float>(front.m_height);
				down_cb.texel[0] = 1.0f / static_cast<float>(source->m_width);
				down_cb.texel[1] = 1.0f / static_cast<float>(source->m_height);
				down_cb.limit[0] = 3.0f;
				update_constant_buffer(g_runtime.m_p_downsample_cb, &down_cb, sizeof(down_cb));

				ID3D11RenderTargetView* front_rtv[1] = { front.m_p_rtv };
				context->OMSetRenderTargets(1, front_rtv, nullptr);
				viewport.Width = static_cast<float>(front.m_width);
				viewport.Height = static_cast<float>(front.m_height);
				context->RSSetViewports(1, &viewport);

				ID3D11Buffer* down_cbs[1] = { g_runtime.m_p_downsample_cb };
				ID3D11ShaderResourceView* source_srvs[1] = { source->m_p_srv };
				context->PSSetConstantBuffers(0, 1, down_cbs);
				context->PSSetShaderResources(0, 1, source_srvs);
				context->PSSetShader(g_runtime.m_p_downsample, nullptr, 0);
				draw_fullscreen();

				context->PSSetShaderResources(0, 1, null_srv);

				// 水平模糊 front -> back
				blur_constants blur_cb = {};
				blur_cb.target[0] = static_cast<float>(back.m_width);
				blur_cb.target[1] = static_cast<float>(back.m_height);
				blur_cb.step[0] = 1.0f / static_cast<float>(front.m_width);
				blur_cb.step[1] = 0.0f;
				update_constant_buffer(g_runtime.m_p_blur_cb, &blur_cb, sizeof(blur_cb));

				ID3D11RenderTargetView* back_rtv[1] = { back.m_p_rtv };
				context->OMSetRenderTargets(1, back_rtv, nullptr);
				viewport.Width = static_cast<float>(back.m_width);
				viewport.Height = static_cast<float>(back.m_height);
				context->RSSetViewports(1, &viewport);

				ID3D11Buffer* blur_cbs[1] = { g_runtime.m_p_blur_cb };
				ID3D11ShaderResourceView* front_srvs[1] = { front.m_p_srv };
				context->PSSetConstantBuffers(0, 1, blur_cbs);
				context->PSSetShaderResources(0, 1, front_srvs);
				context->PSSetShader(g_runtime.m_p_blur, nullptr, 0);
				draw_fullscreen();

				context->PSSetShaderResources(0, 1, null_srv);

				// 垂直模糊 back -> front
				blur_cb.step[0] = 0.0f;
				blur_cb.step[1] = 1.0f / static_cast<float>(back.m_height);
				update_constant_buffer(g_runtime.m_p_blur_cb, &blur_cb, sizeof(blur_cb));

				ID3D11RenderTargetView* front_rtv2[1] = { front.m_p_rtv };
				context->OMSetRenderTargets(1, front_rtv2, nullptr);
				viewport.Width = static_cast<float>(front.m_width);
				viewport.Height = static_cast<float>(front.m_height);
				context->RSSetViewports(1, &viewport);

				ID3D11ShaderResourceView* back_srvs[1] = { back.m_p_srv };
				context->PSSetShaderResources(0, 1, back_srvs);
				context->PSSetShader(g_runtime.m_p_blur, nullptr, 0);
				draw_fullscreen();

				context->PSSetShaderResources(0, 1, null_srv);

				source = &front;
			}
		}

		// ---- 3. 合成到 back buffer ----
		composite_constants composite_cb = {};
		composite_cb.scene_size[0] = static_cast<float>(scene_w);
		composite_cb.scene_size[1] = static_cast<float>(scene_h);
		composite_cb.view[0] = static_cast<float>(g_runtime.m_width);
		composite_cb.view[1] = static_cast<float>(g_runtime.m_height);
		composite_cb.decode[0] = 1.0f;
		composite_cb.decode[1] = g_settings.glow;
		composite_cb.decode[2] = g_settings.fade;
		composite_cb.decode[3] = (scene_w == g_runtime.m_width && scene_h == g_runtime.m_height) ? 1.0f : 0.0f;
		update_constant_buffer(g_runtime.m_p_composite_cb, &composite_cb, sizeof(composite_cb));

		ID3D11RenderTargetView* back_buffer_rtv[1] = { renderer::g_state.m_p_render_target };
		if (back_buffer_rtv[0] == nullptr) return;

		context->OMSetRenderTargets(1, back_buffer_rtv, nullptr);
		viewport.Width = static_cast<float>(g_runtime.m_width);
		viewport.Height = static_cast<float>(g_runtime.m_height);
		context->RSSetViewports(1, &viewport);

		ID3D11Buffer* composite_cbs[1] = { g_runtime.m_p_composite_cb };
		ID3D11ShaderResourceView* composite_srvs[5] =
		{
			g_runtime.m_scene.m_p_srv,
			g_runtime.m_bloom_front[0].m_p_srv,
			g_runtime.m_bloom_front[1].m_p_srv,
			g_runtime.m_bloom_front[2].m_p_srv,
			g_runtime.m_bloom_front[3].m_p_srv,
		};

		context->PSSetConstantBuffers(0, 1, composite_cbs);
		context->PSSetShaderResources(0, 5, composite_srvs);
		context->PSSetShader(g_runtime.m_p_composite, nullptr, 0);
		draw_fullscreen();

		ID3D11ShaderResourceView* null_srvs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
		context->PSSetShaderResources(0, 5, null_srvs);
	}
}
