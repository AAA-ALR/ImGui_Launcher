#pragma once

#include "pch.hpp"

namespace renderer
{
	struct state
	{
		ID3D11Device* m_p_device = nullptr;
		ID3D11DeviceContext* m_p_context = nullptr;
		IDXGISwapChain* m_p_swap_chain = nullptr;
		ID3D11RenderTargetView* m_p_render_target = nullptr;
		ID3D11DepthStencilView* m_p_depth_stencil = nullptr;

		bool m_initialized = false;
		bool m_occluded = false;
	};

	inline state g_state{};

	bool create(HWND hwnd, int width, int height);
	void destroy();

	// 交换链尺寸变化（WM_SIZE 触发），需要先释放并重建 RTV / DSV
	void resize(int width, int height);

	// 每帧：清屏 → ImGui 绘制 → Present
	void begin_frame(float clear_color[4]);
	void end_frame();

	void set_occluded(bool occluded);
}
