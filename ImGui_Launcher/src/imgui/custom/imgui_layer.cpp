#include "pch.hpp"

#include "imgui/custom/imgui_layer.hpp"

#include "effects/burst_background.hpp"
#include "imgui/custom/button_fx.hpp"
#include "imgui/custom/loading_fx.hpp"
#include "imgui/custom/toast.hpp"
#include "fonts/montserrat_extra_bold.hpp"
#include "pages/page_router.hpp"
#include "window.hpp"

namespace imgui_layer
{
	// 界面基础字号（1.0 倍缩放），实际字号 = k_base_font_size * g_scale
	inline constexpr float k_base_font_size = 18.0f;

	// 标题栏高度（1.0 倍缩放），window.cpp 的拖拽区域也用它
	inline constexpr float k_title_height = 48.0f;

	// 标题字号（1.0 倍缩放），实际字号 = k_title_font_size * g_scale
	inline constexpr float k_title_font_size = 34.0f;

	// 依次尝试的系统字体：微软雅黑 → 等线 → 宋体
	static constexpr const char* s_font_candidates[] =
	{
		"C:\\Windows\\Fonts\\msyh.ttc",
		"C:\\Windows\\Fonts\\msyh.ttf",
		"C:\\Windows\\Fonts\\Deng.ttf",
		"C:\\Windows\\Fonts\\simsun.ttc",
	};

	static bool s_backends_ready = false;

	// 标题字体指针。没加载成功时为 nullptr，调用方回退到常规字体。
	static ImFont* s_p_title_font = nullptr;

	static void load_font(float scale)
	{
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();
		s_p_title_font = nullptr;

		ImFontConfig config = {};
		config.OversampleH = 2;
		config.OversampleV = 1;
		config.PixelSnapH = false;

		const float font_size = k_base_font_size * scale;

		// ---- 0. 界面常规字体（Fonts[0]，中文全字集）----
		// 注意这里不能提前 return，否则标题字体就不会被加载。
		bool ui_font_loaded = false;

		for (const char* path : s_font_candidates)
		{
			if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;

			if (io.Fonts->AddFontFromFileTTF(path, font_size, &config, io.Fonts->GetGlyphRangesChineseFull()) != nullptr)
			{
				ui_font_loaded = true;
				break;
			}
		}

		// 全部字体都缺失时退回内置字体（内置字体没有中文，会显示方块，但程序不会崩）
		if (!ui_font_loaded)
			io.Fonts->AddFontDefault(&config);

		// ---- 1. 标题字体（Fonts[1]，只含 ASCII，用于面板标题与加载动画）----
		// 字体字节内嵌在 src/fonts/montserrat_extra_bold.hpp 里，运行期不读盘。
		// 这是 ExtraBold 的 ASCII 子集（0x20-0x7E），44 KB 压到 47 KB 的一半以下。
		// 标题是英文，用默认字集就够，图集占用也最小。
		ImFontConfig title_config = {};
		title_config.OversampleH = 2;
		title_config.OversampleV = 2;
		title_config.PixelSnapH = false;

		// AddFontFromMemoryTTF 默认接管缓冲区所有权，析构时会 free；
		// 这段是 constexpr 静态数组，必须关掉所有权，否则程序退出时崩。
		title_config.FontDataOwnedByAtlas = false;

		s_p_title_font = io.Fonts->AddFontFromMemoryTTF(
			const_cast<unsigned char*>(font_data::k_montserrat_extra_bold),
			font_data::k_montserrat_extra_bold_size,
			k_title_font_size * scale,
			&title_config,
			io.Fonts->GetGlyphRangesDefault());
	}

	ImFont* title_font()
	{
		return s_p_title_font;
	}

	bool init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, float scale)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.IniFilename = nullptr; // 不在磁盘上留 imgui.ini

		load_font(scale);

		// 界面整体压在背景特效之上，背景由 HLSL 负责，所以窗口不留不透明底色
		ImGui::StyleColorsDark();

		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 0.0f;
		style.WindowBorderSize = 0.0f;
		style.FrameRounding = 4.0f;
		style.ScrollbarRounding = 4.0f;
		style.GrabRounding = 4.0f;
		style.WindowPadding = ImVec2(24.0f * scale, 24.0f * scale);
		style.FramePadding = ImVec2(10.0f * scale, 6.0f * scale);
		style.ItemSpacing = ImVec2(10.0f * scale, 8.0f * scale);

		// 完全透空，让下面的 HLSL 背景直接露出来
		style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
		style.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.06f, 0.08f, 0.94f);

		if (!ImGui_ImplWin32_Init(hwnd)) return false;
		if (!ImGui_ImplDX11_Init(device, context)) return false;

		s_backends_ready = true;
		return true;
	}

	void shutdown()
	{
		if (!s_backends_ready) return;

		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		s_backends_ready = false;
	}

	void rebuild_fonts(float scale)
	{
		if (!s_backends_ready) return;
		load_font(scale);
	}

	void begin()
	{
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		button_fx::begin_frame();
		loading_fx::begin_frame();
	}

	void end()
	{
		ImGui::Render();
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	}

	void draw_title_bar(float scale)
	{
		const ImGuiIO& io = ImGui::GetIO();

		const ImGuiWindowFlags flags =
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoScrollbar |
			ImGuiWindowFlags_NoSavedSettings |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoNavFocus;

		ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, k_title_height * scale));

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * scale, 12.0f * scale));
		ImGui::Begin("##title", nullptr, flags);

		ImGui::TextUnformatted(u8"ImGui Launcher");

		// 帧率紧跟标题，分隔符只是视觉上的区分
		ImGui::SameLine();
		ImGui::TextDisabled(u8"|");
		ImGui::SameLine();
		ImGui::TextDisabled("%.1f FPS", io.Framerate);

		// 右上角关闭。窗口没有系统标题栏，所以自己画一颗圆形按钮。
		const float button_size = 28.0f * scale;
		const float margin = 14.0f * scale;
		const float button_x = io.DisplaySize.x - margin - button_size;
		const float button_y = (k_title_height * scale - button_size) * 0.5f;
		const ImVec2 button_min(button_x, button_y);
		const ImVec2 button_max(button_x + button_size, button_y + button_size);

		ImGui::SetCursorScreenPos(button_min);
		ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		const bool close_pressed = ImGui::InvisibleButton("##close", ImVec2(button_size, button_size));
		ImGui::PopStyleColor();

		const bool close_hovered = ImGui::IsItemHovered();
		const bool close_held = ImGui::IsItemActive();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		const float rounding = button_size * 0.5f;
		ImVec4 fill(1.0f, 1.0f, 1.0f, close_hovered ? 0.10f : 0.06f);
		if (close_hovered)
			fill = close_held ? ImVec4(0.72f, 0.16f, 0.24f, 1.0f) : ImVec4(0.92f, 0.28f, 0.36f, 0.95f);

		draw_list->AddRectFilled(button_min, button_max, ImGui::GetColorU32(fill), rounding);

		const float inset = 9.0f * scale;
		const float thickness = 1.7f * scale;
		const ImU32 mark = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, close_hovered ? 1.0f : 0.82f));
		draw_list->AddLine(ImVec2(button_min.x + inset, button_min.y + inset),
			ImVec2(button_max.x - inset, button_max.y - inset), mark, thickness);
		draw_list->AddLine(ImVec2(button_max.x - inset, button_min.y + inset),
			ImVec2(button_min.x + inset, button_max.y - inset), mark, thickness);

		if (close_pressed && window::hwnd != nullptr)
			PostMessageW(window::hwnd, WM_CLOSE, 0, 0);

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void draw(float scale, float pointer_x, float pointer_y, bool pointer_inside)
	{
		burst_background::set_pointer(pointer_x, pointer_y, pointer_inside);

		draw_title_bar(scale);
		pages::draw(scale, pages::g_state);
		toast::draw(scale);
	}

	void handle_window_drag(HWND hwnd)
	{
		if (hwnd == nullptr) return;

		const ImGuiIO& io = ImGui::GetIO();
		if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

		// 光标在控件上（按钮、输入框、滑块…）就交给 ImGui，不抢去做窗口拖动。
		// IsAnyItemHovered 覆盖的都是带 ID 的交互项；纯文本（标题、说明文字）
		// 没有 ID，不算命中，所以面板空白处和文字上都能拖。
		if (ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive()) return;

		// 系统拖动只认非客户区命中，所以先合成一次 HTCAPTION 下的左键按下。
		// 必须先 ReleaseCapture：ImGui 后端在 WM_LBUTTONDOWN 时 SetCapture 了，
		// 不放开的话系统拖动收不到鼠标移动。
		ReleaseCapture();
		SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);

		// 拖动结束后系统仍在窗口上有捕获，放开它，否则下一次悬停收不到
		// WM_MOUSEMOVE，ImGui 的悬停状态会一直停在拖动前的位置。
		ReleaseCapture();

		// 拖动期间 ImGui 收不到 WM_LBUTTONUP，不手动补一次会把左键状态卡在按下。
		if (ImGui::GetIO().MouseDown[0])
			ImGui::GetIO().AddMouseButtonEvent(0, false);
	}
}
