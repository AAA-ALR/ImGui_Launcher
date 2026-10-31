#include "pch.hpp"

#include "imgui/custom/button_fx.hpp"
#include "imgui/custom/widgets.hpp"

namespace custom
{
	namespace
	{
		// 颜色混合，t = 0 取 a，t = 1 取 b
		ImVec4 blend(const ImVec4& a, const ImVec4& b, float t)
		{
			return ImVec4(
				a.x + (b.x - a.x) * t,
				a.y + (b.y - a.y) * t,
				a.z + (b.z - a.z) * t,
				a.w + (b.w - a.w) * t);
		}

		ImVec4 with_alpha(const ImVec4& color, float alpha)
		{
			ImVec4 result = color;
			result.w *= alpha;
			return result;
		}

		// 每帧向目标值逼近，用于悬停 / 聚焦动画。dt 由 ImGui 的帧间隔给。
		float approach(float current, float target, float speed, float delta)
		{
			const float step = std::clamp(speed * delta, 0.0f, 1.0f);
			return current + (target - current) * step;
		}

		// 浮动标签的动画进度存在 ImGuiID 上。不用 unordered_map 是为了避免每帧分配，
		// 控件数量很少，线性查找足够。
		struct anim_entry
		{
			ImGuiID m_id = 0;
			float m_value = 0.0f;
		};

		std::vector<anim_entry>& anim_table()
		{
			static std::vector<anim_entry> s_entries;
			return s_entries;
		}

		float& anim_value(ImGuiID id)
		{
			std::vector<anim_entry>& entries = anim_table();

			for (anim_entry& entry : entries)
			{
				if (entry.m_id == id) return entry.m_value;
			}

			entries.push_back(anim_entry{ id, 0.0f });
			return entries.back().m_value;
		}

	}

	bool button(std::string_view label, ImVec2 size, float scale, const button_style& style)
	{
		ImGui::PushID(label.data(), label.data() + label.size());

		const float height = style.m_height * scale;
		const ImVec2 final_size(size.x, size.y > 0.0f ? size.y : height);

		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 max(min.x + final_size.x, min.y + final_size.y);

		// 导航高亮是一圈和自绘框对不齐的蓝线，按钮外观改由着色器负责。
		ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		const bool pressed = ImGui::InvisibleButton("##button", final_size);
		ImGui::PopStyleColor();

		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		const float rounding = final_size.y * std::clamp(style.m_rounding_ratio, 0.0f, 0.5f);
		const float delta = ImGui::GetIO().DeltaTime;

		// 悬停 / 按下各自缓动。状态挂在 ImGuiID 上，同屏多个按钮互不影响。
		float& hover_anim = anim_value(ImGui::GetID("##hover"));
		hover_anim = approach(hover_anim, hovered ? 1.0f : 0.0f, 10.0f, delta);
		const float s_hover = hover_anim;

		float& press_anim = anim_value(ImGui::GetID("##press"));
		press_anim = approach(press_anim, active ? 1.0f : 0.0f, 16.0f, delta);

		button_fx::desc fx = {};
		fx.m_min = min;
		fx.m_max = max;
		fx.m_rounding = rounding;
		fx.m_border = style.m_border * scale;
		fx.m_hover = s_hover;
		fx.m_press = press_anim;
		fx.m_time = static_cast<float>(ImGui::GetTime());
		fx.m_glow_pad = 22.0f * scale;
		fx.m_accent = style.m_accent;

		// 着色器没就绪时退回一笔圆角矩形，按钮仍然能点。
		if (!button_fx::submit(draw_list, fx))
		{
			const ImVec4 fill = blend(theme::k_surface, style.m_accent, 0.18f + s_hover * 0.20f);
			draw_list->AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
			draw_list->AddRect(min, max, ImGui::GetColorU32(style.m_accent), rounding,
				ImDrawFlags_None, style.m_border * scale);
		}

		// ---- 文字：居中。按下时整体下沉 1px，和原生按钮手感一致 ----
		const ImVec2 text_size = ImGui::CalcTextSize(label.data(), label.data() + label.size());
		const float offset_y = active ? 1.0f * scale : 0.0f;

		const ImVec2 text_pos(
			std::floor((min.x + max.x - text_size.x) * 0.5f),
			std::floor((min.y + max.y - text_size.y) * 0.5f) + offset_y);

		ImVec4 text_color = blend(with_alpha(style.m_text_color, 0.86f),
			ImVec4(1.0f, 1.0f, 1.0f, 1.0f), s_hover);

		// 文字底下留一点光，读起来更像霓虹
		ImVec4 text_glow = style.m_accent;
		text_glow.w = 0.20f + s_hover * 0.15f;
		draw_list->AddText(ImVec2(text_pos.x, text_pos.y + 1.0f * scale),
			ImGui::GetColorU32(text_glow), label.data(), label.data() + label.size());

		draw_list->AddText(text_pos, ImGui::GetColorU32(text_color),
			label.data(), label.data() + label.size());

		ImGui::PopID();
		return pressed;
	}

	input_result input(std::string_view id, std::string_view label, std::string* value,
		float scale, bool password, const input_style& style)
	{
		input_result result;

		ImGui::PushID(id.data(), id.data() + id.size());

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		const float height = style.m_height * scale;
		const float lift = style.m_label_lift * scale;
		const float padding_x = 12.0f * scale;
		const float text_line = ImGui::GetTextLineHeight();

		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;

		// 输入框本体从标签行下面开始，标签浮起来之后不会和文字撞在一起
		const float frame_top = cursor.y + lift;
		const float frame_bottom = frame_top + height;

		const ImVec2 frame_min(cursor.x, frame_top);
		const ImVec2 frame_max(cursor.x + width, frame_bottom);

		// ---- InputText：只借它的输入逻辑，背景和装饰全部自己画 ----
		// 控件矩形必须和下面画的圆角框重合。以前光标又下移了一截 FramePadding，
		// 聚焦时 ImGui 的 NavCursor 画在框外面，底边就多出一条对不齐的蓝线。
		const float frame_padding_y = std::max(0.0f, (height - text_line) * 0.5f);
		const float rounding = 8.0f * scale;

		ImGui::SetCursorScreenPos(frame_min);
		ImGui::SetNextItemWidth(width);

		ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding_x, frame_padding_y));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);

		ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
		if (password) flags |= ImGuiInputTextFlags_Password;

		// 文字和光标走上层，底色走下层。填色若画在 InputText 之后，会把光标盖住。
		draw_list->ChannelsSplit(2);
		draw_list->ChannelsSetCurrent(1);

		result.m_submitted = ImGui::InputText("##input", value, flags);
		result.m_changed = ImGui::IsItemEdited();

		const bool focused = ImGui::IsItemActive();
		const bool hovered = ImGui::IsItemHovered();

		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(4);

		draw_list->ChannelsSetCurrent(0);

		// ---- 动画：聚焦或已有内容时标签浮起 ----
		float& anim = anim_value(ImGui::GetID("##label_anim"));
		const float target = (focused || !value->empty()) ? 1.0f : 0.0f;
		anim = approach(anim, target, 12.0f, ImGui::GetIO().DeltaTime);

		const float glow = approach(focused ? 1.0f : (hovered ? 0.45f : 0.0f), target, 1.0f, 0.0f);
		(void)glow;

		// ---- 输入区底色：一个浅色圆角矩形，只在聚焦时明显 ----
		const ImVec4 surface = theme::k_surface;
		const ImVec4 surface_color = blend(
			ImVec4(surface.x, surface.y, surface.z, surface.w * 0.55f),
			ImVec4(surface.x, surface.y, surface.z, surface.w * 0.95f),
			anim);

		draw_list->AddRectFilled(frame_min, frame_max, ImGui::GetColorU32(surface_color), rounding);

		// ---- 下划线：贴在圆角框的直底边上，左右收到圆角切点，避免穿出弯角 ----
		// 线宽取较粗的那条，整段笔画都留在框内，不会在框外再挂出一条。
		const float accent_thickness = style.m_underline * scale * 1.4f;
		const float underline_y = frame_bottom - accent_thickness * 0.5f;
		const float line_left = frame_min.x + rounding;
		const float line_right = frame_max.x - rounding;

		const ImVec4 base_line = with_alpha(theme::k_text_dim, 0.35f);
		draw_list->AddLine(
			ImVec2(line_left, underline_y),
			ImVec2(line_right, underline_y),
			ImGui::GetColorU32(base_line), style.m_underline * scale);

		const float half = (line_right - line_left) * 0.5f * std::clamp(anim, 0.0f, 1.0f);
		if (half > 0.5f)
		{
			const float center_x = (line_left + line_right) * 0.5f;

			ImVec4 line_color = blend(with_alpha(style.m_accent, 0.85f), ImVec4(1.0f, 1.0f, 1.0f, 1.0f), focused ? 0.25f : 0.0f);
			draw_list->AddLine(ImVec2(center_x - half, underline_y), ImVec2(center_x + half, underline_y),
				ImGui::GetColorU32(line_color), accent_thickness);
		}

		// ---- 浮动标签 ----
		const float rest_y = frame_top + (height - text_line) * 0.5f;
		const float float_y = frame_top - lift * 0.85f;

		const float label_y = rest_y + (float_y - rest_y) * anim;
		const float label_size = ImGui::GetFontSize() * (1.0f - 0.18f * anim);

		ImVec4 label_color = blend(style.m_text_dim, style.m_accent, anim);
		if (focused) label_color = blend(label_color, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.15f);

		draw_list->AddText(ImGui::GetFont(), label_size, ImVec2(frame_min.x + padding_x, label_y),
			ImGui::GetColorU32(label_color), label.data(), label.data() + label.size());

		draw_list->ChannelsMerge();

		// 输入区整体高度 = 标签行 + 输入行，光标留给下一个控件
		ImGui::SetCursorScreenPos(ImVec2(cursor.x, frame_bottom));
		ImGui::Dummy(ImVec2(width, 0.0f));

		ImGui::PopID();
		return result;
	}
}
