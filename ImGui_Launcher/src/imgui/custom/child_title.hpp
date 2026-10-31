#pragma once

#include "pch.hpp"

// 自写控件：面板标题。
//
// 放在 src/imgui/custom 下，与第三方 ImGui 源码同目录树但互不覆盖。
// 这里是本项目自己的代码，遵守 coding-style.md；src/imgui 根目录下的
// imgui*.cpp / imgui*.h 是第三方原版，只读不改。
//
// 绘制方式：
//   1. 用当前字体量出文字尺寸，靠上居中排好
//   2. 文字用垂直渐变（上亮下淡），比纯色更有体积感
//   3. 把这段文字的顶点区间交给 title_glow，由 HLSL 做真正的后期辉光
//
// 发光为什么不在 ImGui 里画：DrawList 只有硬边填充，叠再多层也是同心环，
// 光晕和字形没关系。后期高斯是唯一能出「贴字发光」质感的做法。
//
// 字号由调用方通过 ImGui::PushFont(font, size) 决定，控件本身只负责排布和配色。

namespace custom
{
	struct title_style
	{
		// 渐变顶部颜色（文字上半部分）
		ImVec4 m_color_top = ImVec4(0.98f, 0.97f, 1.0f, 1.0f);

		// 渐变底部颜色（文字下半部分），比顶部暗一档形成竖向过渡
		ImVec4 m_color_bottom = ImVec4(0.72f, 0.62f, 0.94f, 1.0f);

		// 发光颜色。实际发光在 title_glow 里合成，这里只决定强度开关。
		ImVec4 m_glow_color = ImVec4(0.62f, 0.42f, 1.0f, 1.0f);

		// 发光强度，0 = 不发光（也就不会提交给后期层）
		float m_glow_strength = 1.0f;

		// 光晕向外扩散的距离（1.0 倍缩放，像素）。band 也按它往外扩。
		float m_glow_spread = 26.0f;

		// 标题下方留白（1.0 倍缩放，像素），调用方不用再 Spacing
		float m_bottom_gap = 2.0f;
	};

	// 在当前窗口里靠上居中画一个标题，并吃掉对应的布局高度。
	// 必须在一帧内、Begin/End 之间调用。scale 用于随窗口缩放。
	void child_title(std::string_view text, float scale, const title_style& style = {});
}
