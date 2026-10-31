#include "pch.hpp"

#include "window.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace window
{
	void query_screen_size()
	{
		DEVMODEW mode{};
		mode.dmSize = sizeof(mode);
		if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
			mode.dmPelsWidth > 0 &&
			mode.dmPelsHeight > 0)
		{
			screen_w = static_cast<int>(mode.dmPelsWidth);
			screen_h = static_cast<int>(mode.dmPelsHeight);
			return;
		}

		screen_w = GetSystemMetrics(SM_CXSCREEN);
		screen_h = GetSystemMetrics(SM_CYSCREEN);
	}

	float compute_scale()
	{
		// 以 1080p 为 1.0 倍、2160p 为 2.0 倍做线性插值，超出区间夹紧
		if (screen_h <= 0) return k_min_scale;

		const float res_scale = 1.0f +
			(static_cast<float>(screen_h) - k_base_screen_h) /
			(k_uhd_screen_h - k_base_screen_h);

		return std::clamp(res_scale, k_min_scale, k_max_scale);
	}

	void apply_scale()
	{
		query_screen_size();
		g_scale = compute_scale();

		size.x = k_base_width * g_scale;
		size.y = k_base_height * g_scale;

		if (hwnd == nullptr) return;

		// 分辨率变化后重新居中
		const int width = static_cast<int>(size.x);
		const int height = static_cast<int>(size.y);
		const int pos_x = screen_w / 2 - width / 2;
		const int pos_y = screen_h / 2 - height / 2;

		SetWindowPos(hwnd, nullptr, pos_x, pos_y, width, height, SWP_NOZORDER);
	}

	bool create(const wchar_t* class_name, const wchar_t* title)
	{
		// 必须在任何窗口创建之前声明 DPI 感知，配合 per-monitor v2 让 1 逻辑像素 = 1 物理像素
		SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

		// 缩放依赖分辨率，因此先算缩放再决定窗口大小
		apply_scale();

		WNDCLASSEXW window_class = {};
		window_class.cbSize = sizeof(WNDCLASSEXW);
		window_class.style = CS_HREDRAW | CS_VREDRAW;
		window_class.lpfnWndProc = wnd_proc;
		window_class.hInstance = GetModuleHandleW(nullptr);
		window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
#ifdef IDI_ICON1
		window_class.hIcon = LoadIconW(window_class.hInstance, MAKEINTRESOURCEW(IDI_ICON1));
#else
		window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
#endif
		window_class.hIconSm = window_class.hIcon;
		window_class.lpszClassName = class_name;
		window_class.hbrBackground = nullptr; // D3D11 自己清屏，不需要 GDI 背景刷

		if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			return false;

		const int width = static_cast<int>(size.x);
		const int height = static_cast<int>(size.y);

		hwnd = CreateWindowExW(
			WS_EX_APPWINDOW,
			class_name,
			title,
			WS_POPUP,
			screen_w / 2 - width / 2,
			screen_h / 2 - height / 2,
			width,
			height,
			nullptr,
			nullptr,
			window_class.hInstance,
			nullptr);

		if (hwnd == nullptr) return false;

		// Windows 11 圆角 + 系统投影；Win10 上这两句无副作用
		const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

		MARGINS margins = { 1, 1, 1, 1 };
		DwmExtendFrameIntoClientArea(hwnd, &margins);

		ShowWindow(hwnd, SW_SHOWDEFAULT);
		UpdateWindow(hwnd);

		return true;
	}

	void destroy()
	{
		if (hwnd != nullptr)
		{
			DestroyWindow(hwnd);
			hwnd = nullptr;
		}
	}

	int run_message_loop()
	{
		MSG msg = {};
		while (GetMessageW(&msg, nullptr, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return static_cast<int>(msg.wParam);
	}

	LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam))
			return true;

		switch (msg)
		{
		case WM_NCHITTEST:
		{
			// 整窗拖动走 imgui_layer::handle_window_drag()，在 ImGui 帧内按
			// 「本帧有没有控件被悬停」判断，比在这里按坐标猜要准。
			// 所以这里不再把顶部 48px 改成 HTCAPTION：一旦改成非客户区，
			// 之后的鼠标消息全变成 WM_NCxxx，ImGui 就收不到悬停和点击了。
			return DefWindowProcW(hwnd, msg, wparam, lparam);
		}
		case WM_SETCURSOR:
			// 让 ImGui 自己管理光标形状
			if (LOWORD(lparam) == HTCLIENT && ImGui::GetCurrentContext() != nullptr)
				return true;
			break;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		default:
			break;
		}

		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}
