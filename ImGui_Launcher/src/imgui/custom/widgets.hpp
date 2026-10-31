#pragma once

#include "pch.hpp"

// 自写控件：按钮与输入框。
//
// 外观参考 uiverse.io 的两个条目，颜色全部换成本项目的紫色主题：
//   button  ← HLSL 圆角 SDF：玻璃底 + 霓虹描边 + 外晕 + 悬停扫光
//   input   ← Satwinder04/pink-bat-77   浮动标签 + 下划线从左到右展开
//
// 两个控件都不使用 ImGui 的 Frame 绘制（ImGuiCol_Button / FrameBg 那套），
// 完全由我们的代码画，所以换主题只要改下面的 style 默认值。
// 交互仍然走 InvisibleButton / InputText，键盘导航、Tab 焦点、输入法都不受影响。

namespace custom
{
	// 全局紫色主题。其他控件从这里取色，避免每个文件各写一份常量。
	namespace theme
	{
		// 主色（霓虹紫），对应 #A855F7 附近
		inline const ImVec4 k_accent = ImVec4(0.659f, 0.333f, 0.969f, 1.0f);

		// 悬停 / 激活时的亮一档
		inline const ImVec4 k_accent_bright = ImVec4(0.745f, 0.475f, 1.0f, 1.0f);

		// 按钮与输入框正文
		inline const ImVec4 k_text = ImVec4(0.949f, 0.933f, 1.0f, 1.0f);

		// 次要文字（标签、占位）
		inline const ImVec4 k_text_dim = ImVec4(0.639f, 0.608f, 0.741f, 1.0f);

		// 控件底色（半透明深紫，让面板模糊透上来一点）
		inline const ImVec4 k_surface = ImVec4(0.075f, 0.055f, 0.129f, 0.72f);
	}

	struct button_style
	{
		// 控件高度（1.0 倍缩放，像素）。宽度由调用方给。
		float m_height = 40.0f;

		// 圆角占高度的比例，0.5 = 胶囊
		float m_rounding_ratio = 0.5f;

		// 描边宽度（1.0 倍缩放）
		float m_border = 1.6f;

		// 主色（描边 / 发光 / 文字）
		ImVec4 m_accent = theme::k_accent;

		// 文字颜色
		ImVec4 m_text_color = theme::k_text;
	};

	struct input_style
	{
		// 输入行高度（1.0 倍缩放），不含上方给浮动标签留的那一行
		float m_height = 34.0f;

		// 浮动标签抬起来之后占的高度（1.0 倍缩放）
		float m_label_lift = 18.0f;

		// 下划线基础宽度（1.0 倍缩放）
		float m_underline = 1.4f;

		// 标签与下划线颜色
		ImVec4 m_accent = theme::k_accent;
		ImVec4 m_text_color = theme::k_text;
		ImVec4 m_text_dim = theme::k_text_dim;
	};

	struct input_result
	{
		// 文本被改过（和 InputText 的返回值一致）
		bool m_changed = false;

		// 这一帧按了回车（仅当 style 允许回车提交时）
		bool m_submitted = false;
	};

	// 霓虹按钮。id 用 label 本身，同屏出现相同文字时请传唯一的 id。
	// 返回 true 表示这一帧被点击（等价于 ImGui::Button）。
	bool button(std::string_view label, ImVec2 size, float scale, const button_style& style = {});

	// 浮动标签输入框。id 只用于控件内部 ID，不会显示。
	// password = true 时按密码框渲染（掩码显示）。
	input_result input(std::string_view id, std::string_view label, std::string* value,
		float scale, bool password = false, const input_style& style = {});
}
