#pragma once

#include "pch.hpp"

// 按钮特效的 HLSL。运行期 D3DCompile，入口名和 button_fx.cpp 对应。
//
// 以前的按钮是 DrawList 里叠圆角矩形：外发光是几层硬边，扫光是一条方带，
// 圆角处会被切出直角，渐变还有色带。这里改成圆角矩形 SDF，描边、内发光、
// 外晕、扫光都是连续函数，不再有台阶。

namespace shader_source
{
	inline const char* const k_button_fx = R"(
cbuffer button_constants : register(b0)
{
	float4 c_rect;    // xy = 左上（像素）, zw = 右下
	float4 c_accent;  // rgb = 主色, a = 描边宽度（像素）
	float4 c_state;   // x = 悬停 0..1, y = 按下 0..1, z = 时间（秒）, w = 圆角半径
	float4 c_screen;  // xy = 帧缓冲尺寸, z = 外发光外扩（像素）
};

struct vs_out
{
	float4 position : SV_Position;
};

// 六个顶点盖住按钮外扩后的矩形。光晕画在框外，四边形必须比按钮大一圈。
vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 min_px = c_rect.xy - c_screen.z;
	float2 max_px = c_rect.zw + c_screen.z;

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

float sd_round_box(float2 p, float2 half_size, float radius)
{
	float2 q = abs(p) - half_size + radius;
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float4 ps_main(vs_out input) : SV_Target
{
	float2 size = max(c_rect.zw - c_rect.xy, float2(1.0, 1.0));
	float2 center = (c_rect.xy + c_rect.zw) * 0.5;
	float2 half_size = size * 0.5;

	float hover = saturate(c_state.x);
	float press = saturate(c_state.y);
	float time = c_state.z;
	float radius = min(c_state.w, min(half_size.x, half_size.y) - 0.5);

	float2 p = input.position.xy - center;
	float sd = sd_round_box(p, half_size - press * 0.8, max(radius - press * 0.8, 0.0));

	float aa = max(fwidth(sd), 0.75);
	float inside = 1.0 - smoothstep(-aa, aa, sd);

	float3 accent = c_accent.rgb;
	float3 hot = lerp(accent, float3(0.93, 0.82, 1.0), 0.38 + hover * 0.28);
	// 右侧略偏青，和背景的棱镜光走同一个方向，描边不会是一块死紫
	float side_tint = saturate(p.x / max(half_size.x, 1.0) * 0.5 + 0.5);
	float3 rim_tint = lerp(accent, float3(0.55, 0.78, 1.0), side_tint * 0.42);
	rim_tint = lerp(rim_tint, hot, hover * 0.35);

	// 外晕只存在于边线外侧。sd <= 0 时 exp(0) 会变成 1，不能拿来填内部。
	float outside = max(sd, 0.0);
	float tight = exp(-outside * lerp(0.62, 0.36, hover));
	float wide = exp(-outside * lerp(0.20, 0.12, hover));
	float bloom = (tight * 0.70 + wide * 0.50) * lerp(0.40, 1.0, hover);
	bloom *= smoothstep(-1.2, 1.0, sd);
	bloom *= 1.0 - smoothstep(c_screen.z * 0.45, c_screen.z, outside);

	float stroke_width = max(c_accent.a, 1.15);
	float stroke = 1.0 - smoothstep(0.0, stroke_width, abs(sd));
	float rim = exp(-abs(sd) * lerp(1.25, 0.80, hover));
	rim *= 1.0 - smoothstep(stroke_width, stroke_width + lerp(4.5, 8.0, hover), abs(sd));

	float ang = atan2(p.y, p.x);
	float comet = pow(saturate(0.5 + 0.5 * cos(ang - time * 1.65)), lerp(26.0, 12.0, hover));
	comet *= stroke * lerp(0.35, 1.0, hover);

	float breath = 0.84 + 0.16 * sin(time * 1.7);
	rim *= lerp(breath, 1.0, hover);

	float2 uv = (input.position.xy - c_rect.xy) / size;
	float edge = saturate(1.0 - min(uv.x, 1.0 - uv.x) / 0.40);
	float3 glass = lerp(float3(0.030, 0.018, 0.060), accent, 0.04 + 0.16 * edge * (0.35 + hover * 0.65));
	float wall = exp(min(sd, 0.0) * lerp(0.42, 0.28, hover));
	glass += accent * wall * (0.05 + hover * 0.16);

	float sweep = frac(time * 0.35);
	float band = uv.x * 0.82 + uv.y * 0.28;
	float sheen = exp(-pow((band - lerp(-0.25, 1.25, sweep)) * 10.0, 2.0));
	sheen *= inside * hover;

	float body_a = inside * lerp(0.46, 0.58, hover);
	float3 rgb = glass * body_a;
	rgb += rim_tint * stroke * lerp(0.70, 1.0, hover);
	rgb += accent * rim * lerp(0.16, 0.42, hover);
	rgb += accent * bloom * 0.75;
	rgb += hot * comet * 0.80;
	rgb += hot * sheen * 0.28;
	rgb *= lerp(1.0, 0.82, press);

	float alpha = saturate(body_a + stroke * (1.0 - body_a) * 0.92);
	return float4(rgb, alpha);
}
)";
}
