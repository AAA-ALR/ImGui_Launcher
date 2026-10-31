#include "pch.hpp"

#include "imgui/custom/loading.hpp"

#include "imgui/custom/loading_fx.hpp"
#include "imgui/custom/title_glow.hpp"

// 启动加载动画。一轮只有一个方向：
//   光辉从左扫到右，强度始终一样；
//   扫出文字后光辉消失，字母回到暗；
//   停一拍，再从左边以同样强度重新开始。
// 不是左右来回，也不是越扫越淡。
//
// 竖条和字形光都走 HLSL。竖条在字母之前画，光在 ImGui 结束之后加法叠回去，
// 所以字形本身仍然是锐的。

namespace custom
{
	namespace
	{
		// 从左扫到右的时间。强度在这段里不变。
		constexpr float k_sweep_duration = 2.2f;

		// 光辉离开文字之后，整行保持暗的时间，然后周期从头再来。
		constexpr float k_dark_hold = 0.6f;

		constexpr float k_cycle = k_sweep_duration + k_dark_hold;
		constexpr float k_trough_alpha = 0.36f;
		constexpr float k_stripe_step = 8.0f;

		struct letter_state
		{
			float m_alpha = 0.0f;
			float m_scale = 1.0f;
			float m_offset_y = 0.0f;
			float m_glow = 0.0f;
		};

		std::vector<std::string_view> split_glyphs(std::string_view text)
		{
			std::vector<std::string_view> glyphs;
			size_t offset = 0;

			while (offset < text.size())
			{
				const unsigned char lead = static_cast<unsigned char>(text[offset]);
				size_t length = 1;

				if ((lead & 0xE0) == 0xC0) length = 2;
				else if ((lead & 0xF0) == 0xE0) length = 3;
				else if ((lead & 0xF8) == 0xF0) length = 4;

				length = std::min(length, text.size() - offset);
				glyphs.push_back(text.substr(offset, length));
				offset += length;
			}

			return glyphs;
		}

		float smooth_envelope(float distance, float radius)
		{
			const float t = std::clamp(1.0f - distance / std::max(radius, 1.0f), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}
	}

	void loading_screen(std::string_view text, float scale, float time)
	{
		if (text.empty()) return;

		const ImVec2 window_pos = ImGui::GetWindowPos();
		const ImVec2 window_size = ImGui::GetWindowSize();
		const ImVec2 center(window_pos.x + window_size.x * 0.5f, window_pos.y + window_size.y * 0.5f);

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		float base_size = ImGui::GetFontSize();

		const std::vector<std::string_view> glyphs = split_glyphs(text);
		const char* text_begin = text.data();
		const char* text_end = text.data() + text.size();

		float total_width = font->CalcTextSizeA(base_size, FLT_MAX, 0.0f, text_begin, text_end).x;
		const float max_width = window_size.x * 0.78f;
		if (total_width > max_width && total_width > 1.0f)
		{
			base_size *= max_width / total_width;
			total_width = max_width;
		}

		std::vector<float> offsets;
		offsets.reserve(glyphs.size() + 1);
		offsets.push_back(0.0f);

		size_t consumed = 0;
		for (const std::string_view& glyph : glyphs)
		{
			consumed += glyph.size();
			offsets.push_back(font->CalcTextSizeA(base_size, FLT_MAX, 0.0f, text_begin, text_begin + consumed).x);
		}

		total_width = offsets.back();
		const float line_left = std::floor(center.x - total_width * 0.5f);
		const float line_top = std::floor(center.y - base_size * 0.5f);
		const float line_right = line_left + total_width;

		// 一轮：光从文字左侧匀速扫到右侧，强度恒为 1；然后光关掉，字母保持暗。
		const float cycle_time = std::fmod(std::max(time, 0.0f), k_cycle);
		const bool sweeping = cycle_time < k_sweep_duration;
		const float step = k_stripe_step * scale;
		const float radius = step * 4.6f;
		const float travel = total_width + radius * 2.0f;
		const float sweep_t = sweeping ? cycle_time / k_sweep_duration : 1.0f;
		const float sweep_x = line_left - radius + travel * sweep_t;
		const float fade = sweeping ? 1.0f : 0.0f;

		const float band_top = line_top - base_size * 1.15f;
		const float band_bottom = line_top + base_size * 2.35f;

		loading_fx::desc stripes = {};
		stripes.m_min = ImVec2(line_left - step, band_top);
		stripes.m_max = ImVec2(line_right + step, band_bottom);
		stripes.m_sweep_x = sweep_x;
		stripes.m_radius = radius;
		stripes.m_fade = fade;
		stripes.m_step = step;
		stripes.m_text_left = line_left;
		stripes.m_text_right = line_right;
		loading_fx::submit(draw_list, stripes);

		std::vector<ImDrawVert> glow_vertices;
		std::vector<ImDrawIdx> glow_indices;

		for (size_t i_index = 0; i_index < glyphs.size(); ++i_index)
		{
			const std::string_view& glyph = glyphs[i_index];
			letter_state state = {};

			const float heart_x = line_left + (offsets[i_index] + offsets[i_index + 1]) * 0.5f;
			const float hit = sweeping ? smooth_envelope(std::fabs(heart_x - sweep_x), radius) : 0.0f;
			state.m_alpha = k_trough_alpha + (1.0f - k_trough_alpha) * hit;
			state.m_glow = hit;
			state.m_scale = 1.0f + 0.08f * hit;
			state.m_offset_y = -2.0f * hit;

			if (state.m_alpha <= 0.004f) continue;

			const float letter_size = base_size * state.m_scale;
			const float letter_y = line_top + state.m_offset_y * scale;
			const ImVec2 glyph_size = font->CalcTextSizeA(letter_size, FLT_MAX, 0.0f,
				glyph.data(), glyph.data() + glyph.size());
			const float draw_x = heart_x - glyph_size.x * 0.5f;

			const int vertex_begin = draw_list->VtxBuffer.Size;
			const int index_begin = draw_list->IdxBuffer.Size;

			ImVec4 color = ImVec4(0.90f, 0.90f, 0.93f, 1.0f);
			color.w = state.m_alpha;
			draw_list->AddText(font, letter_size, ImVec2(draw_x, letter_y),
				ImGui::GetColorU32(color), glyph.data(), glyph.data() + glyph.size());

			if (state.m_glow <= 0.08f) continue;

			const int vertex_end = draw_list->VtxBuffer.Size;
			const int index_end = draw_list->IdxBuffer.Size;
			const int base = static_cast<int>(glow_vertices.size());

			for (int i_vertex = vertex_begin; i_vertex < vertex_end; ++i_vertex)
			{
				ImDrawVert vertex = draw_list->VtxBuffer[i_vertex];
				const int alpha = static_cast<int>(std::clamp(state.m_glow, 0.0f, 1.0f) * 255.0f);
				vertex.col = (vertex.col & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
				glow_vertices.push_back(vertex);
			}

			for (int i_index_vertex = index_begin; i_index_vertex < index_end; ++i_index_vertex)
			{
				const int local = static_cast<int>(draw_list->IdxBuffer[i_index_vertex]) - vertex_begin + base;
				glow_indices.push_back(static_cast<ImDrawIdx>(local));
			}
		}

		if (!glow_vertices.empty() && !glow_indices.empty())
		{
			const float spread = 18.0f * scale;
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			const float band_x = std::max(0.0f, std::floor(line_left - spread));
			const float band_y = std::max(0.0f, std::floor(line_top - spread));
			const float band_r = std::min(display.x, std::ceil(line_right + spread));
			const float band_b = std::min(display.y, std::ceil(line_top + base_size + spread));

			title_glow::set_tint(1.0f, 1.0f, 1.0f);
			title_glow::set_intensity(1.35f);
			title_glow::submit(glow_vertices.data(), 0, static_cast<int>(glow_vertices.size()),
				glow_indices.data(), 0, static_cast<int>(glow_indices.size()),
				ImVec4(band_x, band_y, band_r - band_x, band_b - band_y));
		}
	}
}
