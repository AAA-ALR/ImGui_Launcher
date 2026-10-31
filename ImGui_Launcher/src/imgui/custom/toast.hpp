#pragma once

#include "pch.hpp"

// 左下角提示。任意线程都能 push，UI 线程在 draw() 里取出再画。
// 不要在工作线程里碰 ImGui。边框颜色就是状态色：信息蓝、成功绿、警告黄、错误红。

namespace toast
{
	enum class e_kind
	{
		e_info,
		e_success,
		e_warning,
		e_error,
	};

	// 不写时长时用这个。单位毫秒。
	inline constexpr int k_default_duration_ms = 5000;

	// 内容、状态、时长（毫秒）。时长省略或 <= 0 时用 5000。
	void push(std::string_view text, e_kind kind, int duration_ms = k_default_duration_ms);

	// ImGui 帧内调用。画在前景层，盖住面板。
	void draw(float scale);
}
