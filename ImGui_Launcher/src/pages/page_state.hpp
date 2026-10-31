#pragma once

#include "pch.hpp"

// 三个页面共用的状态。页面之间只通过这个结构体交流，
// 不持有任何 D3D 资源，也不直接调用渲染器。

namespace pages
{
	enum class e_page
	{
		e_loading,
		e_login,
		e_register,
		e_profile,
	};

	// 启动时加载页的默认文案。运行中的文案在 page_state::m_loading_text，可被工作线程改掉。
	inline constexpr const char* k_loading_text = "Loading";

	struct page_state
	{
		// 启动先进加载页。只有 m_auto_leave 为真时，动画播完才自动进登录页。
		// 登录等操作切进来时会关掉自动离开，由调用方切回去。
		e_page m_current = e_page::e_loading;

		// 进入加载页之前所在的页面。leave_loading() 回到这里。
		e_page m_return_page = e_page::e_login;

		// 加载页文案。启动是 "Loading"，登录操作可以改成 "Logging"。
		std::string m_loading_text = k_loading_text;

		// 启动那一次播完自己离开。工作线程切进来之后为 false。
		bool m_auto_leave = true;

		// 加载页已经显示的秒数，由 page_router 每帧累加。每次进入加载页都会归零。
		float m_loading_time = 0.0f;

		// 表单内容（imgui_stdlib 的 InputText 直接绑定 std::string）
		std::string m_username;
		std::string m_password;
		std::string m_confirm;

		bool m_logged_in = false;

		// 面板矩形（客户区像素，左上原点），由 page_router 每帧算好，
		// 背景模糊模块要用同一个矩形，所以放在状态里共享。
		ImVec4 m_panel = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

		// 面板圆角半径（像素）。模糊层按同一个半径裁切，圆角才不会变直角。
		float m_panel_rounding = 0.0f;
	};

	// 全局唯一实例，风格上等同于单例但不需要 getter
	inline page_state g_state{};
}
