#pragma once

#include "pch.hpp"

#include "pages/page_state.hpp"

// 页面路由：算出面板位置，再按当前页分发绘制。
// 新增页面时只需要在 e_page 里加一项、在 draw() 里加一个分支。

namespace pages
{
	// 根据窗口尺寸与缩放系数计算居中面板的矩形，写入 state.m_panel
	void layout(float scale, page_state& state);

	// 画当前页面。必须在 ImGui::NewFrame() 之后调用。
	void draw(float scale, page_state& state);

	// 下面几个函数任意线程都能调。它们只是把命令推进队列，
	// 下一帧由 UI 线程在 draw() 开头应用，所以不会和 ImGui 抢同一份状态。
	//
	// 工作线程里的典型顺序：
	//   pages::show_loading("Logging");
	//   // 登录请求，不要碰 ImGui
	//   pages::show_page(e_page::e_profile);   // 或 leave_loading() 回到进来之前的页

	// 记住当前页，切到加载页，并把标题改成 text。动画从左边重新开始。
	void show_loading(std::string_view text);

	// 直接切到某一页。从加载页切走时不会自动再跳回来。
	void show_page(e_page page);

	// 从加载页回到 show_loading() 之前的那一页。
	void leave_loading();

	// 登录请求的结果。只在工作线程调用，下一帧才改登录态和页面。
	void complete_login(bool ok);

	// ---- 各页面实现 ----

	void draw_login(float scale, page_state& state);
	void draw_register(float scale, page_state& state);
	void draw_profile(float scale, page_state& state);
}
