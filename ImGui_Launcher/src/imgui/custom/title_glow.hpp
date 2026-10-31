#pragma once

#include "pch.hpp"

// 标题文字的背景发光：HLSL 后期辉光。
//
// 为什么不用 ImGui 画：
//   ImGui 的 DrawList 只能画实心几何（矩形、圆、贴图四边形），没有柔边填充，
//   也没有「先糊一层再叠上去」的能力。之前那版是把圆角矩形由内向外叠 64 次，
//   本质上是用硬边台阶模拟渐变，外缘必然有同心环，而且光晕形状和字形毫无关系
//   —— 那才是「质感不好」的根源。
//
// 现在的做法是真正的后期：
//   1. child_title 把这一帧标题的顶点/索引抄一份交过来（就两段 memcpy）
//   2. 用字体图集把这些顶点画进一张半分辨率的遮罩纹理（R8）
//   3. 水平 / 垂直各一次 9 tap 高斯，得到平滑的光晕
//   4. 加法混合回 back buffer，按 band 边缘羽化，避免露出生硬的矩形边界
//
// 顺序必须是「ImGui 画完文字」之后，否则光会被文字盖住；
// 加法混合对已经在屏幕上的亮字只会更亮，不会糊掉，所以文字本身仍然是 ImGui
// 画的锐利版本，这里只补周围那一圈光。

namespace title_glow
{
	// 创建资源。width / height 是窗口客户区尺寸（和 back buffer 一致）。
	bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);

	void shutdown();

	// 把 child_title 这一帧画出来的顶点交给发光层。
	//   vertex_begin / vertex_end   ImDrawList 顶点数组里的区间
	//   index_begin  / index_end    索引数组里的区间（偏移量，不是指针）
	//   band                        back buffer 上的采样区域（像素，左上原点）
	//   origin                      顶点坐标所在的屏幕原点，一般是 0,0
	void submit(const ImDrawVert* vertices, int vertex_begin, int vertex_end,
		const ImDrawIdx* indices, int index_begin, int index_end,
		const ImVec4& band);

	// 每帧一次，在 imgui_layer::end() 之后、renderer::end_frame() 之前调用。
	// 参数是字体图集的 SRV（ImDrawData 里每张纹理都用它）。
	void render(ID3D11ShaderResourceView* p_atlas);

	// 发光强度倍率，1.0 = 默认
	void set_intensity(float intensity);

	// 光晕颜色。标题用紫色，加载页峰值字母用白色。必须在 submit 之前设。
	void set_tint(float red, float green, float blue);
}
