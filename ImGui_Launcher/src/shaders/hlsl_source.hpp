#pragma once

#include "pch.hpp"

// 把 HLSL 源码按行号缝进字符串，保证着色器内编译错误能定位到具体行。
// 只在 burst_background.cpp 内部使用。

namespace shader_source
{
	inline const char* const k_fullscreen = R"(
struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
	vs_out output;
	output.position = float4(corner, 0.0, 1.0);
	return output;
}
)";

	inline const char* const k_burst = R"(
cbuffer burst_constants : register(b0)
{
	float4 c_res_time;
	float4 c_intensity;
	float4 c_shape;
	float4 c_counts;
	float4 c_offset;
	float4 c_pointer;
	float4 c_intro;
	float4 c_pulses[4];
	float4 c_orient0;
	float4 c_orient1;
	float4 c_orient2;
};

Texture2D<float4> u_gradient : register(t0);
SamplerState u_gradient_sampler : register(s0);

struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
	vs_out output;
	output.position = float4(corner, 0.0, 1.0);
	return output;
}

float hash21(float2 p)
{
	p = floor(p);
	float f = 52.9829189 * frac(dot(p, float2(0.065, 0.005)));
	return frac(f);
}

float2 rot30_apply(float2 v)
{
	return float2(0.8 * v.x + 0.5 * v.y, -0.5 * v.x + 0.8 * v.y);
}

float2 rot2_apply(float2 v, float a)
{
	float s = sin(a);
	float c = cos(a);
	return float2(c * v.x + s * v.y, -s * v.x + c * v.y);
}

float2 twist_apply(float2 v, float a)
{
	float s = sin(a);
	float c = cos(a);
	return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}

float3 orient_apply(float3 v)
{
	return float3(dot(c_orient0.xyz, v), dot(c_orient1.xyz, v), dot(c_orient2.xyz, v));
}

float layered_noise(float2 frag_px)
{
	float2 p = fmod(frag_px + float2(c_res_time.z * 30.0, -c_res_time.z * 21.0), 1024.0);
	float2 q = rot30_apply(p);

	float n = 0.0;
	n += 0.40 * hash21(q);
	n += 0.25 * hash21(q * 2.0 + 17.0);
	n += 0.20 * hash21(q * 4.0 + 47.0);
	n += 0.10 * hash21(q * 8.0 + 113.0);
	n += 0.05 * hash21(q * 16.0 + 191.0);
	return n;
}

float edge_fade(float2 frag, float2 res, float2 offset)
{
	float2 to_center = frag - 0.5 * res - offset;
	float r = length(to_center) / (0.5 * min(res.x, res.y) * c_shape.w);
	float x = saturate(r);

	float q = x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
	float s = q * 0.5;
	s = pow(s, 1.5);

	float tail = 1.0 - pow(1.0 - s, 2.0);
	s = lerp(s, tail, 0.2);

	float dn = (layered_noise(frag * 0.15) - 0.5) * 0.0015 * s;
	return saturate(s + dn);
}

float bend_angle(float3 q, float t)
{
	return 0.8 * sin(q.x * 0.55 + t * 0.6)
		+ 0.7 * sin(q.y * 0.50 - t * 0.5)
		+ 0.6 * sin(q.z * 0.60 + t * 0.7);
}

float4 ps_main(vs_out input) : SV_Target
{
	float2 res = c_res_time.xy;
	float2 frag = input.position.xy;
	float t = c_res_time.w;

	float jitter_amp = 0.1 * saturate(c_intensity.z);
	float2 centered = 2.0 * (frag - c_offset.xy) - res;
	float3 dir = normalize(float3(centered, res.y * max(c_intensity.w, 1e-3)));

	float march_t = 0.0;
	float3 col = 0.0;

	float n = layered_noise(frag);
	float amp = clamp(c_intensity.y, 0.0, 50.0) * 0.15;
	float edge = 0.5 - 0.35 * (c_shape.x - 1.0);

	float twist_amount = c_shape.y;
	float twist_angle = 0.0;
	if (twist_amount != 0.0)
		twist_angle = twist_amount * length(centered) / res.y;

	[loop]
	for (int step_index = 0; step_index < 44; ++step_index)
	{
		float3 p = march_t * dir;
		p.z -= 2.0;
		float rad = length(p);

		float3 pl = p * (10.0 / max(rad, 1e-6));
		if (twist_amount != 0.0)
			pl.xy = twist_apply(pl.xy, twist_angle);
		pl = orient_apply(pl);

		float step_len = min(rad - 0.3, n * jitter_amp) + 0.1;

		float3 pb = pl;
		if (amp > 0.0)
		{
			float grow = smoothstep(0.35, 3.0, march_t);
			float a1 = amp * grow * bend_angle(pl * 0.6, t);
			float a2 = 0.5 * amp * grow * bend_angle(pl.zyx * 0.5 + 3.1, t * 0.9);
			pb.xz = rot2_apply(pb.xz, a1);
			pb.xy = rot2_apply(pb.xy, a2);
		}

		float ray_pattern = smoothstep(
			edge,
			edge + 0.2,
			sin(pb.x + cos(pb.y) * cos(pb.z)) * sin(pb.z + sin(pb.y) * cos(pb.x + t)));

		if (c_counts.x > 0)
		{
			float angle = atan2(pb.y, pb.x);
			float comb = 0.5 + 0.5 * cos((float)c_counts.x * angle);
			comb = pow(comb, 3.0);
			ray_pattern *= smoothstep(0.15, 0.95, comb);
		}

		float depth = march_t - c_shape.z;
		float3 spectral;
		if (c_counts.y > 0)
		{
			float saw = frac(depth * 0.25);
			float t_ray = saw * saw * (3.0 - 2.0 * saw);
			spectral = 2.0 * u_gradient.SampleLevel(u_gradient_sampler, float2(saturate(t_ray), 0.5), 0).rgb;

			// 渐变纹理只采样到紫色附近就饱和，避免高光整体压向白色
			spectral = lerp(spectral, spectral * spectral, 0.35);
		}
		else
		{
			spectral = 1.0 + cos(depth * 3.0 + float3(0.0, 1.0, 2.0));
		}

		float radial_falloff = 1.0 - smoothstep(0.0, 5.0, rad);
		col += (0.05 / (0.4 + step_len)) * radial_falloff * spectral * ray_pattern;

		march_t += step_len;
	}

	col *= edge_fade(frag, res, c_offset.xy);
	col *= c_intensity.x;

	float unit = 0.5 * min(res.x, res.y);
	float radius = length(frag - 0.5 * res - c_offset.xy) / unit;

	float lift = 1.0;
	[unroll]
	for (int pulse_index = 0; pulse_index < 4; ++pulse_index)
	{
		float4 pulse = c_pulses[pulse_index];
		if (pulse.y <= 0.0)
			continue;

		float d = (radius - pulse.x) / pulse.z;
		lift += pulse.y * exp(-d * d) + pulse.w;
	}

	if (c_pointer.z > 0.0)
	{
		float2 m = (frag - c_pointer.xy) / unit;
		lift += c_pointer.z * exp(-dot(m, m) / max(c_pointer.w * c_pointer.w, 1e-4));
	}

	if (c_intro.z > 0.0)
	{
		float2 to_center = frag - 0.5 * res - c_offset.xy;
		float angle = atan2(to_center.y, to_center.x);
		float streak = 0.5
			+ 0.22 * sin(angle * 5.0 + 1.3)
			+ 0.16 * sin(angle * 13.0 + 4.1)
			+ 0.12 * sin(angle * 29.0 + 2.2);
		float front = c_intro.x * (0.7 + 0.3 * streak);
		float d = (radius - front) / 0.2;
		lift += c_intro.y * exp(-d * d);
		lift *= 1.0 - smoothstep(front - 0.5, front, radius);
	}

	return float4(col * lift * c_counts.z, 1.0);
}
)";

	inline const char* const k_downsample = R"(
cbuffer downsample_constants : register(b0)
{
	float4 c_target;
	float4 c_texel;
	float4 c_limit;
};

Texture2D<float4> u_source : register(t0);
SamplerState u_sampler : register(s0);

struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
	vs_out output;
	output.position = float4(corner, 0.0, 1.0);
	return output;
}

float4 ps_main(vs_out input) : SV_Target
{
	float2 uv = input.position.xy / c_target.xy;
	float2 texel = c_texel.xy;

	float3 color = u_source.SampleLevel(u_sampler, uv - texel, 0).rgb;
	color += u_source.SampleLevel(u_sampler, uv + texel, 0).rgb;
	color += u_source.SampleLevel(u_sampler, uv + float2(texel.x, -texel.y), 0).rgb;
	color += u_source.SampleLevel(u_sampler, uv + float2(-texel.x, texel.y), 0).rgb;

	return float4(min(color * 0.25, c_limit.xxx), 1.0);
}
)";

	inline const char* const k_blur = R"(
cbuffer blur_constants : register(b0)
{
	float4 c_target;
	float4 c_step;
};

Texture2D<float4> u_source : register(t0);
SamplerState u_sampler : register(s0);

struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
	vs_out output;
	output.position = float4(corner, 0.0, 1.0);
	return output;
}

float4 ps_main(vs_out input) : SV_Target
{
	float2 uv = input.position.xy / c_target.xy;
	float2 step_uv = c_step.xy;

	float3 color = u_source.SampleLevel(u_sampler, uv, 0).rgb * 0.181862;
	color += (u_source.SampleLevel(u_sampler, uv + step_uv * 1.423135, 0).rgb
		+ u_source.SampleLevel(u_sampler, uv - step_uv * 1.423135, 0).rgb) * 0.284316;
	color += (u_source.SampleLevel(u_sampler, uv + step_uv * 3.326702, 0).rgb
		+ u_source.SampleLevel(u_sampler, uv - step_uv * 3.326702, 0).rgb) * 0.106598;
	color += (u_source.SampleLevel(u_sampler, uv + step_uv * 5.242989, 0).rgb
		+ u_source.SampleLevel(u_sampler, uv - step_uv * 5.242989, 0).rgb) * 0.018156;

	return float4(color, 1.0);
}
)";

	inline const char* const k_composite = R"(
cbuffer composite_constants : register(b0)
{
	float4 c_scene_size;
	float4 c_view;
	float4 c_decode;
};

Texture2D<float4> u_scene : register(t0);
Texture2D<float4> u_bloom0 : register(t1);
Texture2D<float4> u_bloom1 : register(t2);
Texture2D<float4> u_bloom2 : register(t3);
Texture2D<float4> u_bloom3 : register(t4);
SamplerState u_sampler : register(s0);

struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
	vs_out output;
	output.position = float4(corner, 0.0, 1.0);
	return output;
}

float bicubic_weight0(float f) { return f * (-0.5 + f * (1.0 - 0.5 * f)); }
float bicubic_weight1(float f) { return 1.0 + f * f * (-2.5 + 1.5 * f); }
float bicubic_weight2(float f) { return f * (0.5 + f * (2.0 - 1.5 * f)); }
float bicubic_weight3(float f) { return f * f * (-0.5 + 0.5 * f); }

float3 sample_bicubic(float2 uv)
{
	float2 scene_size = c_scene_size.xy;
	float2 position = uv * scene_size;
	float2 center = floor(position - 0.5) + 0.5;
	float2 f = position - center;

	float2 w0 = float2(bicubic_weight0(f.x), bicubic_weight0(f.y));
	float2 w1 = float2(bicubic_weight1(f.x), bicubic_weight1(f.y));
	float2 w2 = float2(bicubic_weight2(f.x), bicubic_weight2(f.y));
	float2 w3 = float2(bicubic_weight3(f.x), bicubic_weight3(f.y));
	float2 w12 = w1 + w2;

	float2 t0 = (center - 1.0) / scene_size;
	float2 t3 = (center + 2.0) / scene_size;
	float2 t12 = (center + w2 / w12) / scene_size;

	float3 result = u_scene.SampleLevel(u_sampler, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;
	result += u_scene.SampleLevel(u_sampler, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;
	result += u_scene.SampleLevel(u_sampler, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;
	result += u_scene.SampleLevel(u_sampler, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;
	result += u_scene.SampleLevel(u_sampler, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;
	result += u_scene.SampleLevel(u_sampler, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;
	result += u_scene.SampleLevel(u_sampler, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;
	result += u_scene.SampleLevel(u_sampler, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;
	result += u_scene.SampleLevel(u_sampler, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;

	return max(result, 0.0);
}

float smootherstep01(float t)
{
	t = saturate(t);
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float4 ps_main(vs_out input) : SV_Target
{
	float2 uv = input.position.xy / c_view.xy;

	float3 col = (c_decode.w > 0.5)
		? u_scene.Load(int3(int2(input.position.xy), 0)).rgb
		: sample_bicubic(uv);

	float bloom_strength = c_decode.y;
	if (bloom_strength > 0.0)
	{
		float3 halo = u_bloom0.SampleLevel(u_sampler, uv, 0).rgb * 0.42;
		halo += u_bloom1.SampleLevel(u_sampler, uv, 0).rgb * 0.3;
		halo += u_bloom2.SampleLevel(u_sampler, uv, 0).rgb * 0.18;
		halo += u_bloom3.SampleLevel(u_sampler, uv, 0).rgb * 0.1;
		col += halo * bloom_strength;
	}

	col = max(col * c_decode.x, 0.0);

	// 网页版把 alpha 写成 max(rgb) 交给 canvas 预乘合成，只有超过 1 的部分被压回去，
	// 中间调原样上屏。这里用同样的思路：knee 以下完全不动，超出部分按 1/(1+x) 收敛。
	// 之前先 saturate 再走 Reinhard 等于把所有亮度整体乘掉，画面自然比网页暗一档。
	const float k_knee = 0.8;
	const float k_softness = 2.0;
	float peak = max(max(col.r, col.g), col.b);
	if (peak > k_knee)
	{
		float excess = peak - k_knee;
		col *= (k_knee + excess / (1.0 + excess * k_softness)) / peak;
	}

	float edge = c_decode.z * 0.5;
	if (edge > 0.0)
	{
		float mask = smootherstep01(uv.x / edge) * smootherstep01((1.0 - uv.x) / edge);
		mask *= smootherstep01(uv.y / edge) * smootherstep01((1.0 - uv.y) / edge);
		col *= mask;
	}

	return float4(col, 1.0);
}
)";

	// 面板背后的高斯模糊。两个 pass 共用这段代码，靠 c_step 的 xy / yx 区分方向。
	// c_rect 与 c_round 只在最终拷贝时用到：把模糊结果限制在圆角面板内，
	// 圆角半径参与距离计算，否则模糊层会从外面把 ImGui 的圆角补成直角。
	inline const char* const k_backdrop = R"(
cbuffer backdrop_constants : register(b0)
{
	float4 c_texel;   // xy = 目标尺寸, zw = 1 / 目标尺寸
	float4 c_step;    // xy = uv 步长, z = 半径倍率, w = 羽化宽度
	float4 c_rect;    // xy = 面板左上（像素）, zw = 面板宽高
	float4 c_round;   // x = 圆角半径（像素）
};

Texture2D<float4> u_source : register(t0);
SamplerState u_sampler : register(s0);

struct vs_out
{
	float4 position : SV_Position;
};

vs_out vs_main(uint vertex_id : SV_VertexID)
{
	float2 corner = float2((vertex_id == 1u) ? 3.0 : -1.0, (vertex_id == 2u) ? 3.0 : -1.0);
	vs_out output;
	output.position = float4(corner, 0.0, 1.0);
	return output;
}

// 9 tap 可分离高斯，权重来自 sigma ≈ 2.2 的归一化系数
static const float k_weights[5] =
{
	0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162
};

float4 ps_blur(vs_out input) : SV_Target
{
	// uv 按目标像素坐标归一化，保证整张目标覆盖完整的源图（目标分辨率低于源）
	float2 uv = input.position.xy * c_texel.zw;
	float2 offset = c_step.xy;

	float3 sum = u_source.SampleLevel(u_sampler, uv, 0).rgb * k_weights[0];
	[unroll]
	for (int i_index = 1; i_index < 5; ++i_index)
	{
		sum += u_source.SampleLevel(u_sampler, uv + offset * i_index, 0).rgb * k_weights[i_index];
		sum += u_source.SampleLevel(u_sampler, uv - offset * i_index, 0).rgb * k_weights[i_index];
	}

	return float4(sum, 1.0);
}

// 圆角矩形有符号距离：内部为负，边界为 0，外部为正。
// 注意参数名不能用 point —— 那是 HLSL 几何着色器的图元关键字。
float rounded_box_distance(float2 local, float2 half_size, float radius)
{
	float2 q = abs(local) - (half_size - radius);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// 把模糊图按圆角面板裁剪，圆角外保持原样（alpha = 0，交给混合保留底色）
float4 ps_composite(vs_out input) : SV_Target
{
	float2 uv = input.position.xy * c_texel.zw;
	float3 blurred = u_source.SampleLevel(u_sampler, uv, 0).rgb;

	float2 half_size = c_rect.zw * 0.5;
	float2 center = c_rect.xy + half_size;
	float radius = min(c_round.x, min(half_size.x, half_size.y));

	// 用圆角矩形距离而不是轴对齐矩形，圆角处才能和 ImGui 对齐
	float distance_to_edge = -rounded_box_distance(input.position.xy - center, half_size, radius);

	float feather = max(c_step.w, 1.0);
	float mask = saturate(distance_to_edge / feather + 0.5);
	return float4(blurred * mask, mask);
}
)";
}
