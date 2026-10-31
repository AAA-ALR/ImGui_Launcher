#pragma once

#include "pch.hpp"

// 标题发光的 HLSL。单独一个头文件，避免 hlsl_source.hpp 继续膨胀。
// 全部走 D3DCompile 运行期编译，入口名和 title_glow.cpp 里的字符串一一对应。
//
// 坐标约定：band 是屏幕上一块矩形（像素，左上原点），中间目标的大小正好等于 band，
// 所以「band 局部 uv」和「目标 uv」是同一个东西。合成阶段画到整窗时再把屏幕像素
// 换算回 band 局部坐标，落在 0..1 之外的直接输出 0。

namespace shader_source
{
	// vs_fullscreen   全屏三角形，给 blur / composite 用
	// vs_mask         把 ImDrawVert 的屏幕像素坐标映射进 band 局部裁剪空间
	// ps_mask         从字体图集取 alpha，写进 R8 遮罩
	// ps_blur         9 tap 可分离高斯，靠 c_step.xy 区分方向
	// ps_composite    加法混合回 back buffer，按 band 边缘羽化
	inline const char* const k_title_glow = R"(
cbuffer glow_constants : register(b0)
{
float4 c_band;     // xy = band 左上（像素）, zw = band 尺寸
float4 c_texel;    // xy = 全屏目标的像素尺寸, zw = 1 / 尺寸（合成阶段用）
float4 c_step;     // xy = uv 步长, z = 强度倍率
float4 c_tint;     // rgb = 光晕颜色
float4 c_feather;  // x = 羽化宽度（像素）
};

Texture2D<float4> u_source : register(t0);
SamplerState u_sampler : register(s0);

struct vs_out
{
float4 position : SV_Position;
float2 uv : TEXCOORD0;
};

// 全屏三角形：三个顶点覆盖整个 viewport，不需要顶点缓冲
vs_out vs_fullscreen(uint vertex_id : SV_VertexID)
{
float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
vs_out output;
output.position = float4(corner, 0.0, 1.0);
output.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
return output;
}

// 遮罩顶点：输入是 ImGui 的屏幕像素坐标，减 band 左上再除以 band 尺寸，
// 得到 0..1 的局部坐标，再换算到裁剪空间 [-1,1]。
struct mask_in
{
float2 pos : POSITION;
float2 uv : TEXCOORD0;
float4 col : COLOR0;
};

struct mask_out
{
float4 position : SV_Position;
float2 uv : TEXCOORD0;
float4 col : COLOR0;
};

mask_out vs_mask(mask_in input)
{
mask_out output;
float2 local = (input.pos - c_band.xy) / c_band.zw;
output.position = float4(local.x * 2.0 - 1.0, 1.0 - local.y * 2.0, 0.0, 1.0);
output.uv = input.uv;
output.col = input.col;
return output;
}

// 只取 alpha：字体图集是白色字形 + alpha 覆盖率
float ps_mask(mask_out input) : SV_Target
{
return u_source.Sample(u_sampler, input.uv).a * input.col.a;
}

// 9 tap 可分离高斯，sigma ≈ 2.2
static const float k_glow_weights[5] =
{
0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162
};

float ps_blur(vs_out input) : SV_Target
{
float2 uv = input.uv;
float2 offset = c_step.xy;

float sum = u_source.SampleLevel(u_sampler, uv, 0).r * k_glow_weights[0];
[unroll]
for (int i_index = 1; i_index < 5; ++i_index)
{
sum += u_source.SampleLevel(u_sampler, uv + offset * i_index, 0).r * k_glow_weights[i_index];
sum += u_source.SampleLevel(u_sampler, uv - offset * i_index, 0).r * k_glow_weights[i_index];
}

return sum;
}

// 合成：加法混合。屏幕像素先换算到 band 局部坐标，band 外的像素直接不发光，
// band 四边再做一次软化，避免生硬的矩形边界。
float4 ps_composite(vs_out input) : SV_Target
{
// input.uv 是整窗的 0..1；转成像素再相对 band 求局部坐标
float2 pixel = input.uv * c_texel.xy;
float2 local = (pixel - c_band.xy) / c_band.zw;

if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0)
return 0.0;

float halo = u_source.SampleLevel(u_sampler, local, 0).r;

float2 distance_to_edge = min(local, 1.0 - local) * c_band.zw;
float edge_mask = saturate(min(distance_to_edge.x, distance_to_edge.y) / max(c_feather.x, 1.0));

float strength = halo * c_step.z * edge_mask;
return float4(c_tint.rgb * strength, strength);
}
)";
}
