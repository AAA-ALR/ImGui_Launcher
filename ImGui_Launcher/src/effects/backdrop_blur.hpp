#pragma once

#include "pch.hpp"

// 中心面板背后的高斯模糊。
//
// 做法：把已经画好的 back buffer 整张拷进离屏纹理，降到 1/4 分辨率，
// 水平 / 垂直各做一次 9 tap 高斯，再按面板矩形裁切贴回 back buffer。
// 因为降采样一次、模糊两次，等效模糊半径约 24 像素，正好是毛玻璃质感。
//
// 调用顺序固定：
//   burst_background::render()        // 背景先画到 back buffer
//   backdrop_blur::render(x, y, w, h) // 把面板区域的背景糊掉
//   imgui_layer::draw()               // ImGui 顶着模糊层画
//
// 尺寸在 init 时确定，运行期窗口不变，因此不重建任何资源。

namespace backdrop_blur
{
	// 创建离屏纹理与着色器。width / height 是窗口客户区尺寸。
	bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);

	void shutdown();

	// 每帧调用一次：把 back buffer 上 (x, y, width, height) 这块矩形模糊掉。
	// 坐标是客户区像素，左上原点。corner_radius 必须和面板的圆角保持一致，
	// 否则模糊层会把圆角从外面补成直角。
	void render(float x, float y, float width, float height, float corner_radius);

	// 模糊强度倍率，1.0 = 默认。调大后光斑更散，调小更接近原图。
	void set_strength(float strength);
}
