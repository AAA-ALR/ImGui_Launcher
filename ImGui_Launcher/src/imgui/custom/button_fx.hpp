#pragma once

#include "pch.hpp"

// 按钮外观走 HLSL，不走 DrawList 叠矩形。
//
// custom::button 只负责命中和文字。本体（玻璃底、SDF 描边、外晕、扫光）
// 在 ImGui 渲染这条命令时用回调画进 back buffer，文字命令排在回调后面，
// 所以字仍然是 ImGui 的锐利版本，光不会糊到字形上。
//
// 初始化失败时 submit 返回 false，调用方退回一笔纯色圆角矩形。

namespace button_fx
{
	struct desc
	{
		ImVec2 m_min = ImVec2(0.0f, 0.0f);
		ImVec2 m_max = ImVec2(0.0f, 0.0f);
		float m_rounding = 0.0f;
		float m_border = 1.6f;
		float m_hover = 0.0f;
		float m_press = 0.0f;
		float m_time = 0.0f;
		float m_glow_pad = 18.0f;
		ImVec4 m_accent = ImVec4(0.659f, 0.333f, 0.969f, 1.0f);
	};

	bool init(ID3D11Device* device, ID3D11DeviceContext* context);
	void shutdown();

	// 每帧开始清掉上一帧的实例。必须在任何 submit 之前调用。
	void begin_frame();

	// 登记一个按钮，并在当前 DrawList 上插入绘制回调。
	// 返回 false 表示着色器没就绪或本帧名额已满，调用方需要自己画底。
	bool submit(ImDrawList* draw_list, const desc& item);
}
