#pragma once

#include "pch.hpp"

// 加载页光斑。对应 uiverse dexter-st/wicked-elephant-4 的 loader::after：
// 五层径向渐变叠成彩虹，再用 repeating-linear-gradient 切成竖条。
// 这里直接在像素里算竖条和高斯衰减，不再用一堆矩形去近似。

namespace shader_source
{
	inline const char* const k_loading_fx = R"(
cbuffer loading_constants : register(b0)
{
	float4 c_band;    // xy = 光斑矩形左上, zw = 右下（像素）
	float4 c_sweep;   // x = 扫光中心, y = 光斑半径, z = 整体透明度, w = 竖条间距
	float4 c_span;    // x = 文字左, y = 文字右
	float4 c_screen;  // xy = 帧缓冲尺寸
};

struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 min_px = c_band.xy;
	float2 max_px = c_band.zw;
	float2 xy = min_px;

	if (vertex_id == 1u)
		xy = float2(max_px.x, min_px.y);
	else if (vertex_id == 2u || vertex_id == 4u)
		xy = max_px;
	else if (vertex_id == 5u)
		xy = float2(min_px.x, max_px.y);

	float2 ndc = float2(xy.x / c_screen.x * 2.0 - 1.0, 1.0 - xy.y / c_screen.y * 2.0);
	vs_out output;
	output.position = float4(ndc, 0.0, 1.0);
	return output;
}

float3 hsv_to_rgb(float hue, float sat, float value)
{
	float3 phase = abs(frac(hue + float3(1.0, 0.6666667, 0.3333333)) * 6.0 - 3.0);
	return value * lerp(float3(1.0, 1.0, 1.0), saturate(phase - 1.0), sat);
}

float stripe_energy(float pixel_x, float index, float y_norm, out float3 color)
{
	float step = max(c_sweep.w, 1.0);
	float center = c_span.x + (index + 0.5) * step;
	float dx = pixel_x - center;

	float core = exp(-dx * dx * 1.15);
	float halo = exp(-dx * dx * 0.11) * 0.32;
	float energy = core + halo;

	float along = saturate((center - (c_sweep.x - c_sweep.y)) / max(c_sweep.y * 2.0, 1.0));
	float hue = frac(0.97 - y_norm * 0.58 + along * 0.22 + index * 0.015);
	color = hsv_to_rgb(hue, 0.82, 1.0);
	color = lerp(color, float3(1.0, 0.97, 0.92), core * 0.42);

	float dist = abs(center - c_sweep.x) / max(c_sweep.y, 1.0);
	float envelope = saturate(1.0 - dist);
	envelope = envelope * envelope * (3.0 - 2.0 * envelope);

	float edge = saturate(min(center - c_span.x, c_span.y - center) / max(step * 1.5, 1.0));
	return energy * envelope * edge;
}

float4 ps_main(vs_out input) : SV_Target
{
	float2 pixel = input.position.xy;
	float height = max(c_band.w - c_band.y, 1.0);
	float y_norm = saturate((pixel.y - c_band.y) / height);

	float vertical = smoothstep(0.0, 0.20, y_norm) * smoothstep(1.0, 0.78, y_norm);
	float belly = exp(-pow((y_norm - 0.46) * 2.15, 2.0));
	vertical *= 0.28 + 0.72 * belly;

	float step = max(c_sweep.w, 1.0);
	float index = floor((pixel.x - c_span.x) / step);

	float3 color = 0.0;
	float energy = 0.0;
	[unroll]
	for (int i_offset = -1; i_offset <= 1; ++i_offset)
	{
		float3 stripe_color = 0.0;
		float stripe = stripe_energy(pixel.x, index + i_offset, y_norm, stripe_color);
		color += stripe_color * stripe;
		energy += stripe;
	}

	float strength = c_sweep.z * vertical;
	float3 rgb = color * strength;
	return float4(rgb, energy * strength);
}
)";
}
