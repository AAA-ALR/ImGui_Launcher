#pragma once

#include "pch.hpp"

// 窗口基础尺寸（1.0 倍缩放下的逻辑像素）
namespace window
{
	inline constexpr float k_base_width = 900.0f;
	inline constexpr float k_base_height = 560.0f;

	// 缩放区间与参考分辨率：以「屏幕高度」为依据，1080p = 1.0 倍，2160p = 2.0 倍
	inline constexpr float k_min_scale = 0.75f;
	inline constexpr float k_max_scale = 2.0f;
	inline constexpr float k_base_screen_h = 1080.0f;
	inline constexpr float k_uhd_screen_h = 2160.0f;

	// 当前窗口尺寸（已乘 g_scale）
	struct frame_size
	{
		float x = k_base_width;
		float y = k_base_height;
	};

	inline frame_size size{};
	inline HWND hwnd = nullptr;

	// 屏幕尺寸（EnumDisplaySettingsW 优先，取不到再退回 GetSystemMetrics）
	inline int screen_w = 0;
	inline int screen_h = 0;

	// 全工程唯一的缩放系数
	inline float g_scale = 1.0f;

	LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

	// 读取主显示器当前分辨率
	void query_screen_size();

	// 按屏幕高度换算缩放系数，夹在 [k_min_scale, k_max_scale]
	float compute_scale();

	// 重新计算 g_scale 与 size，并把窗口按新尺寸重新居中
	void apply_scale();

	// 注册窗口类 → 应用缩放 → 创建窗口 → 阴影 / 无边框
	bool create(const wchar_t* class_name, const wchar_t* title);

	void destroy();

	// 阻塞式消息循环，返回退出码
	int run_message_loop();
}
