#include "pch.hpp"

#include "imgui/custom/child_title.hpp"

#include "imgui/custom/title_glow.hpp"

// 面板标题控件。
//
// 两件事：
//   1. 渐变文字：ImGui 的 AddText 只接受单色，但字形本身就是四边形，
//      直接把顶点颜色按 y 在「上色 → 下色」之间插值，就能得到竖向渐变，
//      不需要着色器，也不需要多画一遍。
//   2. 发光：把自己刚写进 DrawList 的那段顶点/索引抄给 title_glow，
//      HLSL 在半分辨率遮罩上做高斯，加法混合回 back buffer。
//      这里负责算 band（要采样的屏幕区域），往外扩 m_glow_spread。

namespace custom
{
	namespace
	{
		// 顶点颜色按行插值时的上下范围留一点余量：字体包围盒通常比字形略高，
		// 直接从 y0 插到 y1 会让降到下半部时太快变暗。
		constexpr float k_gradient_bias = 0.15f;
	}

	void child_title(std::string_view text, float scale, const title_style& style)
	{
		if (text.empty()) return;

		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const float avail_width = ImGui::GetContentRegionAvail().x;

		const char* text_begin = text.data();
		const char* text_end = text.data() + text.size();

		// 用当前字体量宽度，所以换字体 / 换字号都不需要改这里
		const ImVec2 text_size = ImGui::CalcTextSize(text_begin, text_end);

		// 水平居中：可用宽度减文字宽度的一半。floor 掉小数，避免半像素让字形发虚。
		const ImVec2 text_pos(
			cursor.x + std::floor((avail_width - text_size.x) * 0.5f),
			cursor.y);

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		const float font_size = ImGui::GetFontSize();

		const int vertex_begin = draw_list->VtxBuffer.Size;
		const int index_begin = draw_list->IdxBuffer.Size;

		draw_list->AddText(font, font_size, text_pos, ImGui::GetColorU32(style.m_color_top),
			text_begin, text_end);

		const int vertex_end = draw_list->VtxBuffer.Size;
		const int index_end = draw_list->IdxBuffer.Size;

		// ---- 竖向渐变：把刚追加的顶点按 y 重新着色 ----
		// AddText 已经把 pos / uv 填好了，这里只改 color，不影响排版和字形。
		const float top = text_pos.y + text_size.y * k_gradient_bias;
		const float bottom = text_pos.y + text_size.y * (1.0f - k_gradient_bias);
		const float span = std::max(bottom - top, 1.0f);

		for (int i_vertex = vertex_begin; i_vertex < vertex_end; ++i_vertex)
		{
			ImDrawVert& vertex = draw_list->VtxBuffer[i_vertex];

			const float t = std::clamp((vertex.pos.y - top) / span, 0.0f, 1.0f);

			const ImVec4 color(
				style.m_color_top.x + (style.m_color_bottom.x - style.m_color_top.x) * t,
				style.m_color_top.y + (style.m_color_bottom.y - style.m_color_top.y) * t,
				style.m_color_top.z + (style.m_color_bottom.z - style.m_color_top.z) * t,
				1.0f);

			vertex.col = ImGui::GetColorU32(color);
		}

		// ---- 交给后期发光 ----
		const float strength = std::clamp(style.m_glow_strength, 0.0f, 1.0f);
		if (strength > 0.0f && text_size.x > 0.0f)
		{
			const float spread = style.m_glow_spread * scale;

			// band 必须覆盖光晕可能出现的整个区域，再往外扩一点给羽化留空间。
			// 竖直方向光晕靠字形四周扩散，所以上下按同样距离扩。
			const ImVec2 display = ImGui::GetIO().DisplaySize;

			const float band_x = std::max(0.0f, std::floor(text_pos.x - spread));
			const float band_y = std::max(0.0f, std::floor(text_pos.y - spread));
			const float band_r = std::min(display.x, std::ceil(text_pos.x + text_size.x + spread));
			const float band_b = std::min(display.y, std::ceil(text_pos.y + text_size.y + spread));

			title_glow::set_tint(0.62f, 0.42f, 1.0f);
			title_glow::submit(
				draw_list->VtxBuffer.Data, vertex_begin, vertex_end,
				draw_list->IdxBuffer.Data, index_begin, index_end,
				ImVec4(band_x, band_y, band_r - band_x, band_b - band_y));

			title_glow::set_intensity(strength);
		}

		// 文字是画出来的，没有走控件布局，所以这里手动吃掉一行高度（含下方留白），
		// 宽度给满可用宽度，让光标停在行首，下一个控件照常左对齐。
		ImGui::Dummy(ImVec2(avail_width, text_size.y + style.m_bottom_gap * scale));
	}
}
