#pragma once

#include "pch.hpp"

// 自写控件：启动加载页。
//
// 动画移植自 uiverse.io 的 dexter-st/wicked-elephant-4。
// 竖条和字形光走 HLSL，字母本身仍由 ImGui 画。
// 调用时必须已经在面板窗口里，动画居中于当前窗口。

namespace custom
{
	// time 是从加载页开始到现在的秒数。
	void loading_screen(std::string_view text, float scale, float time);
}
