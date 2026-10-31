#pragma once

#include "pch.hpp"

// ImGui 层的封装：上下文、后端、字体、标题栏、整窗拖动。
// 只暴露这几样，页面代码不需要知道后端细节。

namespace imgui_layer
{
	// 初始化 ImGui 上下文、Win32 / DX11 后端、字体
	bool init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, float scale);

	void shutdown();

	// 每帧一组的开始 / 结束
	void begin();
	void end();

	// 字体按新的缩放系数重建（apply_scale 之后调用）
	void rebuild_fonts(float scale);

	// 面板标题专用字体（Montserrat ExtraBold，ASCII 子集，内嵌在二进制里）。
	// 加载失败时返回 nullptr，调用方需要判空并回退到默认字体。
	ImFont* title_font();

	// 左上角标题栏：程序名 + 帧率
	void draw_title_bar(float scale);

	// 标题栏 + 当前页面。在 begin() 与 end() 之间调用。
	// 指针坐标一并发给背景特效，避免主循环里再取一次 IO。
	void draw(float scale, float pointer_x, float pointer_y, bool pointer_inside);

	// 整窗拖动：在 draw() 之后调用。本帧按下左键且没有命中任何控件时，
	// 交给系统按标题栏处理，从而实现「除控件外整个窗口都能拖」。
	void handle_window_drag(HWND hwnd);
}
