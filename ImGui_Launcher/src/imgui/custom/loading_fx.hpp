#pragma once

#include "pch.hpp"

// 加载页的彩虹竖条。在 ImGui 画字母之前用回调画进 back buffer，
// 加法混合，所以只加亮、不会盖住后面的字形。

namespace loading_fx
{
	struct desc
	{
		ImVec2 m_min = ImVec2(0.0f, 0.0f);
		ImVec2 m_max = ImVec2(0.0f, 0.0f);
		float m_sweep_x = 0.0f;
		float m_radius = 1.0f;
		float m_fade = 0.0f;
		float m_step = 8.0f;
		float m_text_left = 0.0f;
		float m_text_right = 0.0f;
	};

	bool init(ID3D11Device* device, ID3D11DeviceContext* context);
	void shutdown();
	void begin_frame();

	// 返回 false 表示着色器没就绪，调用方不要再退回矩形竖条。
	bool submit(ImDrawList* draw_list, const desc& item);
}
