#include "pch.hpp"

#include "imgui/custom/toast.hpp"

#include <deque>
#include <mutex>

namespace toast
{
	namespace
	{
		constexpr float k_fade_in = 0.16f;
		constexpr float k_fade_out = 0.22f;
		constexpr std::size_t k_max_visible = 5;

		struct toast_item
		{
			std::string m_text;
			e_kind m_kind = e_kind::e_info;
			float m_duration = 5.0f;
			float m_age = 0.0f;
		};

		std::mutex g_mutex;
		std::deque<toast_item> g_pending;
		std::vector<toast_item> g_active;

		ImVec4 kind_color(e_kind kind)
		{
			switch (kind)
			{
			case e_kind::e_info:
				return ImVec4(0.35f, 0.62f, 1.00f, 1.0f);
			case e_kind::e_success:
				return ImVec4(0.30f, 0.84f, 0.48f, 1.0f);
			case e_kind::e_warning:
				return ImVec4(0.98f, 0.78f, 0.22f, 1.0f);
			case e_kind::e_error:
				return ImVec4(0.96f, 0.34f, 0.38f, 1.0f);
			}

			return ImVec4(0.35f, 0.62f, 1.00f, 1.0f);
		}

		float smooth_step(float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		float item_alpha(const toast_item& item)
		{
			const float in_t = (k_fade_in > 0.0f) ? item.m_age / k_fade_in : 1.0f;
			const float out_t = (k_fade_out > 0.0f)
				? (item.m_duration + k_fade_out - item.m_age) / k_fade_out
				: 1.0f;
			return smooth_step(std::min(in_t, out_t));
		}
	}

	void push(std::string_view text, e_kind kind, int duration_ms)
	{
		if (text.empty())
			return;

		if (duration_ms <= 0)
			duration_ms = k_default_duration_ms;

		toast_item item;
		item.m_text = std::string(text);
		item.m_kind = kind;
		item.m_duration = static_cast<float>(duration_ms) * 0.001f;

		std::lock_guard<std::mutex> lock(g_mutex);
		g_pending.push_back(std::move(item));
	}

	void draw(float scale)
	{
		if (ImGui::GetCurrentContext() == nullptr)
			return;

		const ImGuiIO& io = ImGui::GetIO();
		if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
			return;

		const float delta = std::min(io.DeltaTime, 0.1f);

		{
			std::deque<toast_item> incoming;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				incoming.swap(g_pending);
			}

			for (toast_item& item : incoming)
			{
				bool merged = false;
				for (toast_item& active : g_active)
				{
					if (active.m_kind != item.m_kind || active.m_text != item.m_text)
						continue;

					active.m_duration = item.m_duration;
					active.m_age = 0.0f;
					merged = true;
					break;
				}

				if (!merged)
					g_active.push_back(std::move(item));
			}
		}

		if (g_active.size() > k_max_visible)
			g_active.erase(g_active.begin(), g_active.begin() + static_cast<std::ptrdiff_t>(g_active.size() - k_max_visible));

		for (toast_item& item : g_active)
			item.m_age += delta;

		g_active.erase(
			std::remove_if(g_active.begin(), g_active.end(), [](const toast_item& item)
			{
				return item.m_age >= item.m_duration + k_fade_out;
			}),
			g_active.end());

		if (g_active.empty())
			return;

		ImFont* font = ImGui::GetFont();
		if (font == nullptr)
			return;

		const float font_size = ImGui::GetFontSize();
		const float margin = 18.0f * scale;
		const float gap = 8.0f * scale;
		const float pad_x = 16.0f * scale;
		const float pad_y = 11.0f * scale;
		const float max_width = 340.0f * scale;
		const float wrap = std::max(40.0f, max_width - pad_x * 2.0f);
		const float rounding = 10.0f * scale;
		const float border = 1.75f * scale;

		ImDrawList* draw_list = ImGui::GetForegroundDrawList();
		float y = io.DisplaySize.y - margin;

		// 新的贴着左下角，旧的往上堆。
		for (int i_index = static_cast<int>(g_active.size()) - 1; i_index >= 0; --i_index)
		{
			const toast_item& item = g_active[static_cast<std::size_t>(i_index)];
			const float alpha = item_alpha(item);
			if (alpha <= 0.001f)
				continue;

			const ImVec2 text_size = font->CalcTextSizeA(
				font_size, FLT_MAX, wrap, item.m_text.c_str(), item.m_text.c_str() + item.m_text.size());

			const float box_w = std::min(max_width, text_size.x + pad_x * 2.0f);
			const float box_h = text_size.y + pad_y * 2.0f;

			const float in_t = (k_fade_in > 0.0f) ? std::clamp(item.m_age / k_fade_in, 0.0f, 1.0f) : 1.0f;
			const float slide = (1.0f - smooth_step(in_t)) * 16.0f * scale;
			const float x = margin - slide;
			y -= box_h;

			if (y + box_h < 0.0f)
				break;

			const ImVec2 box_min(x, y);
			const ImVec2 box_max(x + box_w, y + box_h);

			const int shadow_alpha = static_cast<int>(90.0f * alpha);
			draw_list->AddRectFilled(
				ImVec2(box_min.x + 2.0f * scale, box_min.y + 3.0f * scale),
				ImVec2(box_max.x + 2.0f * scale, box_max.y + 4.0f * scale),
				IM_COL32(0, 0, 0, shadow_alpha),
				rounding);

			ImVec4 fill(0.07f, 0.06f, 0.10f, 0.94f * alpha);
			draw_list->AddRectFilled(box_min, box_max, ImGui::GetColorU32(fill), rounding);

			ImVec4 border_color = kind_color(item.m_kind);
			border_color.w *= alpha;
			draw_list->AddRect(box_min, box_max, ImGui::GetColorU32(border_color), rounding, 0, border);

			const ImVec2 text_pos(box_min.x + pad_x, box_min.y + pad_y);
			const ImU32 text_color = ImGui::GetColorU32(ImVec4(0.96f, 0.95f, 0.98f, alpha));
			draw_list->AddText(font, font_size, text_pos, text_color,
				item.m_text.c_str(), item.m_text.c_str() + item.m_text.size(), wrap);

			y -= gap;
		}
	}
}
