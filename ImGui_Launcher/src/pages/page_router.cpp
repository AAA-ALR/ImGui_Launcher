#include "pch.hpp"

#include "pages/page_router.hpp"

#include "effects/burst_background.hpp"
#include "task_queue.hpp"
#include "window.hpp"

#include "misc/fnv1a.hpp"

#include <deque>
#include <mutex>
#include "imgui/custom/child_title.hpp"
#include "imgui/custom/loading.hpp"
#include "imgui/custom/toast.hpp"
#include "imgui/custom/widgets.hpp"
#include "imgui/custom/imgui_layer.hpp"

// 自写控件：标题 / 按钮 / 输入框 / 加载动画都在 src/imgui/custom 下，
// 页面这边只负责调用，不直接碰 DrawList，也不写死任何颜色。

namespace pages
{
	namespace
	{
		enum class e_command
		{
			e_show_loading,
			e_show_page,
			e_leave_loading,
			e_login_result,
		};

		struct page_command
		{
			e_command m_kind = e_command::e_show_page;
			e_page m_page = e_page::e_login;
			std::string m_text;
			bool m_ok = false;
		};

		std::mutex g_command_mutex;
		std::deque<page_command> g_commands;

		void enqueue(page_command command)
		{
			std::lock_guard<std::mutex> lock(g_command_mutex);
			g_commands.push_back(std::move(command));
		}

		void apply_one(page_state& state, const page_command& command)
		{
			switch (command.m_kind)
			{
			case e_command::e_show_loading:
				if (state.m_current != e_page::e_loading)
					state.m_return_page = state.m_current;
				state.m_current = e_page::e_loading;
				state.m_loading_text = command.m_text;
				state.m_loading_time = 0.0f;
				state.m_auto_leave = false;
				break;

			case e_command::e_show_page:
				state.m_current = command.m_page;
				state.m_auto_leave = false;
				break;

			case e_command::e_leave_loading:
				if (state.m_current == e_page::e_loading)
					state.m_current = state.m_return_page;
				state.m_auto_leave = false;
				break;

			case e_command::e_login_result:
				state.m_auto_leave = false;
				if (command.m_ok)
				{
					state.m_logged_in = true;
					state.m_password.clear();
					state.m_current = e_page::e_profile;
					burst_background::trigger();
				}
				else
				{
					state.m_current = state.m_return_page;
					toast::push(u8"登录失败", toast::e_kind::e_error, 5000);
				}
				break;
			}
		}

		void apply_commands(page_state& state)
		{
			std::deque<page_command> local;
			{
				std::lock_guard<std::mutex> lock(g_command_mutex);
				local.swap(g_commands);
			}

			for (const page_command& command : local)
				apply_one(state, command);
		}
	}

	void show_loading(std::string_view text)
	{
		page_command command;
		command.m_kind = e_command::e_show_loading;
		command.m_text = std::string(text);
		enqueue(std::move(command));
	}

	void show_page(e_page page)
	{
		page_command command;
		command.m_kind = e_command::e_show_page;
		command.m_page = page;
		enqueue(std::move(command));
	}

	void leave_loading()
	{
		page_command command;
		command.m_kind = e_command::e_leave_loading;
		enqueue(std::move(command));
	}

	void complete_login(bool ok)
	{
		page_command command;
		command.m_kind = e_command::e_login_result;
		command.m_ok = ok;
		enqueue(std::move(command));
	}

	namespace
	{
		// 面板基础尺寸（1.0 倍缩放），实际尺寸 = 基础 × scale
		// 高度按内容最长的 profile 页留够：标题 34px 字体 + 头像 + 四个控件 + 底部按钮
		constexpr float k_panel_width = 420.0f;
		constexpr float k_panel_height = 500.0f;
		constexpr float k_panel_rounding = 12.0f;

		// 面板描边宽度（1.0 倍缩放）。ImGui 的 WindowBorderSize 为 0 时不画边框，
		// 所以必须显式给一个非零值，边框才会按 ImGuiCol_Border 画出来。
		constexpr float k_panel_border = 1.0f;

		// 只给启动那一次用。一轮扫光大约 3 秒，播完自己进登录页。
		// 登录切进来时 m_auto_leave 是 false，这里不会把它赶走。
		constexpr float k_loading_duration = 3.0f;

		constexpr ImGuiWindowFlags k_panel_flags =
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoSavedSettings;

		// 面板标题：英文 + 靠上居中 + 渐变 + HLSL 辉光。
		// 字号在加载标题字体时就定好了（k_title_font_size * scale），这里只切字体。
		// PushFont 的单参数版本在 1.93 已移除，必须显式给一个字号；
		// LegacySize 就是 AddFontFromMemoryTTF 时传入的字号，等于「保持原样」。
		void draw_page_title(std::string_view text, float scale)
		{
			ImFont* title_font = imgui_layer::title_font();
			if (title_font != nullptr)
				ImGui::PushFont(title_font, title_font->LegacySize);

			custom::child_title(text, scale);

			if (title_font != nullptr)
				ImGui::PopFont();
		}

		// 加载页画在同一块面板里，背后就是已经糊过的背景。播够时间自动进登录页。
		void draw_loading(float scale, page_state& state)
		{
			const ImGuiIO& io = ImGui::GetIO();

			ImFont* title_font = imgui_layer::title_font();
			const float base_size = 48.0f * scale;

			if (title_font != nullptr)
				ImGui::PushFont(title_font, base_size);

			custom::loading_screen(state.m_loading_text, scale, state.m_loading_time);

			if (title_font != nullptr)
				ImGui::PopFont();

			state.m_loading_time += io.DeltaTime;

			if (state.m_auto_leave && state.m_loading_time >= k_loading_duration)
			{
				state.m_current = e_page::e_login;
				state.m_auto_leave = false;
				state.m_loading_time = 0.0f;
			}
		}
	}

	void layout(float scale, page_state& state)
	{
		ImVec2 display = ImGui::GetIO().DisplaySize;
		if (display.x < 1.0f || display.y < 1.0f)
			display = ImVec2(window::size.x, window::size.y);

		const float width = k_panel_width * scale;
		const float height = k_panel_height * scale;

		state.m_panel = ImVec4(
			std::floor((display.x - width) * 0.5f),
			std::floor((display.y - height) * 0.5f),
			width,
			height);

		// 模糊层要用同一个圆角半径，两个地方共用一个值，避免对不齐
		state.m_panel_rounding = k_panel_rounding * scale;
	}

	void draw(float scale, page_state& state)
	{
		// 工作线程推进来的切页 / 改文案，在画之前落到状态上。
		apply_commands(state);

		// 加载页和登录页共用同一块面板，模糊矩形才不会在启动时被清掉。
		layout(scale, state);

		ImGui::SetNextWindowPos(ImVec2(state.m_panel.x, state.m_panel.y));
		ImGui::SetNextWindowSize(ImVec2(state.m_panel.z, state.m_panel.w));

		// 面板本身半透明，露出来的就是背后已经模糊过的背景。
		// alpha 太高会把模糊结果压成一片死色，0.3 左右还能看见光线走向。
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.055f, 0.09f, 0.30f));

		// 描边：WindowBorderSize 大于 0 时 ImGui 才画边框，颜色取 ImGuiCol_Border。
		// 边框跟着 WindowRounding 走圆角，和背景模糊的圆角遮罩是同一个半径。
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.62f, 0.50f, 1.0f, 0.55f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, k_panel_border * scale);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, k_panel_rounding * scale);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f * scale, 26.0f * scale));

		if (ImGui::Begin("##panel", nullptr, k_panel_flags))
		{
			if (state.m_current == e_page::e_loading)
			{
				draw_loading(scale, state);
			}
			else switch (state.m_current)
			{
			case e_page::e_login:
				draw_login(scale, state);
				break;
			case e_page::e_register:
				draw_register(scale, state);
				break;
			case e_page::e_profile:
				draw_profile(scale, state);
				break;
			default:
				break;
			}
		}

		ImGui::End();
		ImGui::PopStyleVar(3);
		ImGui::PopStyleColor(2);
	}

	void draw_login(float scale, page_state& state)
	{
		draw_page_title("Sign In", scale);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		custom::input("login_username", u8"用户名", &state.m_username, scale);

		ImGui::Spacing();

		const custom::input_result password = custom::input(
			"login_password", u8"密码", &state.m_password, scale, true);

		ImGui::Spacing();
		ImGui::Spacing();

		const ImVec2 button_size(ImGui::GetContentRegionAvail().x, 0.0f);
		const bool pressed = custom::button(u8"登录", button_size, scale);

		if (pressed || password.m_submitted)
		{
			if (state.m_username.empty() || state.m_password.empty())
			{
				toast::push(u8"用户名和密码不能为空", toast::e_kind::e_error, 5000);
			}
			else
			{
				// UI 线程只负责切到加载页。请求在线程池里跑，完了再切到个人页。
				const std::string user = state.m_username;
				const std::string pass = state.m_password;
				show_loading("Logging");
				task_queue::push([user, pass]()
				{
					// 占位：这里换成真正的登录请求。不要碰 ImGui，也不要直接改 g_state。
					Sleep(3000);
					const bool ok = !user.empty() && !pass.empty();
					complete_login(ok);
				});
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		custom::button_style secondary;
		secondary.m_height = 30.0f;
		secondary.m_accent = ImVec4(0.55f, 0.48f, 0.78f, 1.0f);

		if (custom::button(u8"还没有账号？去注册", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), scale, secondary))
			state.m_current = e_page::e_register;
	}

	void draw_register(float scale, page_state& state)
	{
		draw_page_title("Create Account", scale);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		custom::input("reg_username", u8"用户名", &state.m_username, scale);

		ImGui::Spacing();
		custom::input("reg_password", u8"密码", &state.m_password, scale, true);

		ImGui::Spacing();
		custom::input("reg_confirm", u8"确认密码", &state.m_confirm, scale, true);

		ImGui::Spacing();
		ImGui::Spacing();

		const ImVec2 button_size(ImGui::GetContentRegionAvail().x, 0.0f);
		if (custom::button(u8"创建账号", button_size, scale))
		{
			if (state.m_username.empty() || state.m_password.empty())
			{
				toast::push(u8"用户名和密码不能为空", toast::e_kind::e_error, 5000);
			}
			else if (state.m_password != state.m_confirm)
			{
				toast::push(u8"两次输入的密码不一致", toast::e_kind::e_warning, 5000);
			}
			else
			{
				// 占位实现：接真正的注册接口时替换这里
				state.m_confirm.clear();
				state.m_current = e_page::e_login;
				toast::push(u8"账号已创建，请登录", toast::e_kind::e_success, 5000);
			}
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		custom::button_style secondary;
		secondary.m_height = 30.0f;
		secondary.m_accent = ImVec4(0.55f, 0.48f, 0.78f, 1.0f);

		if (custom::button(u8"返回登录", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), scale, secondary))
			state.m_current = e_page::e_login;
	}

	void draw_profile(float scale, page_state& state)
	{
		draw_page_title("Profile", scale);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// 头像占位：一个圆形色块 + 用户名首字母。
		// 首字母用标题字体（ExtraBold），和标题同源，识别度更高。
		const float avatar_size = 72.0f * scale;
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		const ImVec2 center(cursor.x + avatar_size * 0.5f, cursor.y + avatar_size * 0.5f);

		// 圆底：内圈亮、外圈淡，比纯色块更像徽章
		draw_list->AddCircleFilled(center, avatar_size * 0.5f, IM_COL32(150, 105, 255, 255), 0);
		draw_list->AddCircleFilled(center, avatar_size * 0.5f,
			ImGui::GetColorU32(ImVec4(0.55f, 0.30f, 0.95f, 0.85f)), 0);

		if (!state.m_username.empty())
		{
			const char initial[2] = { state.m_username[0], '\0' };

			ImFont* title_font = imgui_layer::title_font();
			const float initial_size = avatar_size * 0.46f;

			// 首字母单独用一个字号：标题字体是 ExtraBold，缩到 33px 左右刚好占满圆心
			const ImVec2 text_size = (title_font != nullptr)
				? title_font->CalcTextSizeA(initial_size, FLT_MAX, 0.0f, initial, initial + 1)
				: ImGui::CalcTextSize(initial);

			draw_list->AddText(
				title_font, initial_size,
				ImVec2(std::floor(center.x - text_size.x * 0.5f), std::floor(center.y - text_size.y * 0.5f)),
				IM_COL32(255, 255, 255, 255), initial);
		}

		ImGui::Dummy(ImVec2(avatar_size, avatar_size));

		ImGui::Spacing();
		ImGui::Text("%s", state.m_username.empty() ? u8"未命名用户" : state.m_username.c_str());
		ImGui::TextDisabled("%s", state.m_logged_in ? u8"已登录" : u8"离线");

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// 四种 toast 的手动测试。边框颜色跟按钮主色一致，时长 5000 毫秒。
		{
			const float gap = ImGui::GetStyle().ItemSpacing.x;
			const float button_w = (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f;

			custom::button_style toast_test;
			toast_test.m_height = 30.0f;

			toast_test.m_accent = ImVec4(0.35f, 0.62f, 1.00f, 1.0f);
			if (custom::button(u8"信息", ImVec2(button_w, 0.0f), scale, toast_test))
				toast::push(u8"这是一条信息", toast::e_kind::e_info, 5000);

			ImGui::SameLine();
			toast_test.m_accent = ImVec4(0.30f, 0.84f, 0.48f, 1.0f);
			if (custom::button(u8"成功", ImVec2(button_w, 0.0f), scale, toast_test))
				toast::push(u8"这是一条成功", toast::e_kind::e_success, 5000);

			ImGui::SameLine();
			toast_test.m_accent = ImVec4(0.98f, 0.78f, 0.22f, 1.0f);
			if (custom::button(u8"警告", ImVec2(button_w, 0.0f), scale, toast_test))
				toast::push(u8"这是一条警告", toast::e_kind::e_warning, 5000);

			ImGui::SameLine();
			toast_test.m_accent = ImVec4(0.96f, 0.34f, 0.38f, 1.0f);
			if (custom::button(u8"错误", ImVec2(button_w, 0.0f), scale, toast_test))
				toast::push(u8"这是一条错误", toast::e_kind::e_error, 5000);
		}

		{
			const float gap = ImGui::GetStyle().ItemSpacing.x;
			const float button_w = (ImGui::GetContentRegionAvail().x - gap * 3.0f);

			static std::string fnv1a_input_str;
			custom::input(u8"fnv1a_input", u8"fnv1a str", &fnv1a_input_str, scale);
			if (custom::button(u8"汇编hash算法测试", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), scale))
			{
				std::cout << u8"fnv1a_32: 0x" << std::hex << std::uppercase <<
					misc::fnv1a_32(fnv1a_input_str.c_str()) << std::dec << std::nouppercase << "\n";
				std::cout << u8"fnv1a_32_asm: 0x" << std::hex << std::uppercase << 
					fnv1a_32_asm(fnv1a_input_str.c_str()) << std::dec << std::nouppercase << "\n";
			}
		}

		ImGui::Spacing();

		if (custom::button(u8"触发特效", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), scale))
			burst_background::trigger();

		ImGui::Spacing();

		custom::button_style danger;
		danger.m_height = 34.0f;
		danger.m_accent = ImVec4(0.98f, 0.42f, 0.52f, 1.0f);

		if (custom::button(u8"退出登录", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), scale, danger))
		{
			state.m_logged_in = false;
			state.m_username.clear();
			state.m_current = e_page::e_login;
		}
	}
}
