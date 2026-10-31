#include "pch.hpp"

#include "effects/backdrop_blur.hpp"
#include "effects/burst_background.hpp"
#include "imgui/custom/button_fx.hpp"
#include "imgui/custom/loading_fx.hpp"
#include "imgui/custom/imgui_layer.hpp"
#include "pages/page_router.hpp"
#include "task_queue.hpp"
#include "pages/page_state.hpp"
#include "renderer_dx11.hpp"
#include "imgui/custom/title_glow.hpp"
#include "window.hpp"

#ifdef _DEBUG
static void enable_console()
{
	if (!AllocConsole()) return;

	FILE* fp = nullptr;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);
	freopen_s(&fp, "CONIN$", "r", stdin);
	std::ios::sync_with_stdio();
	std::cout.clear();
	std::cerr.clear();
	std::cin.clear();
	SetConsoleTitleW(L"DEV Console");
	SetConsoleOutputCP(CP_UTF8);
}

static void log_line(std::string_view text)
{
	std::cout << text << '\n';
	std::cout.flush();
}
#else
static void log_line(std::string_view) {}
#endif

static void apply_default_burst_palette()
{
	burst_background::settings& config = burst_background::config();
	config.colors =
	{
		{ 0.658f, 0.333f, 0.969f }, // #A855F7
		{ 0.486f, 0.227f, 0.929f }, // #7C3AED
		{ 0.388f, 0.400f, 0.945f }, // #6366F1
	};
	assert(!config.colors.empty());
}

// 启动信息统一从控制台看，界面里只保留标题和帧率。
// 控制台已切到 CP_UTF8，所以每条中文都必须带 u8 前缀；
// 工程没有 /utf-8，漏写 u8 的普通字面量会是 GBK 字节，打出来就是乱码。
static void log_environment(int width, int height)
{
	char buffer[256] = {};

	snprintf(buffer, sizeof(buffer), u8"屏幕分辨率: %d x %d", window::screen_w, window::screen_h);
	log_line(buffer);

	snprintf(buffer, sizeof(buffer), u8"缩放系数: %.3f", window::g_scale);
	log_line(buffer);

	snprintf(buffer, sizeof(buffer), u8"窗口尺寸: %d x %d", width, height);
	log_line(buffer);

	const burst_background::settings& config = burst_background::config();

	snprintf(buffer, sizeof(buffer), u8"背景分辨率比例: %.2f", config.resolution_scale);
	log_line(buffer);

	snprintf(buffer, sizeof(buffer), u8"配色数量: %zu", config.colors.size());
	log_line(buffer);
}

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
#ifdef _DEBUG
	enable_console();
	log_line(u8"启动 ImGui_Launcher");
#endif

	// 窗口尺寸在启动时算一次，之后不再随分辨率变化
	if (!window::create(L"Launcher", L"Launcher"))
	{
		log_line(u8"窗口创建失败");
		return 1;
	}

	const int width = static_cast<int>(window::size.x);
	const int height = static_cast<int>(window::size.y);

	if (!renderer::create(window::hwnd, width, height))
	{
		log_line(u8"D3D11 初始化失败");
		window::destroy();
		return 2;
	}

	apply_default_burst_palette();
	log_environment(width, height);

	if (!burst_background::init(renderer::g_state.m_p_device, renderer::g_state.m_p_context, width, height))
	{
		log_line(u8"背景特效初始化失败");
		renderer::destroy();
		window::destroy();
		return 3;
	}

	if (!backdrop_blur::init(renderer::g_state.m_p_device, renderer::g_state.m_p_context, width, height))
	{
		log_line(u8"面板模糊初始化失败");
		burst_background::shutdown();
		renderer::destroy();
		window::destroy();
		return 4;
	}

	// 标题辉光：后期高斯。失败不致命，标题照常显示，只是没有光晕。
	if (!title_glow::init(renderer::g_state.m_p_device, renderer::g_state.m_p_context, width, height))
		log_line(u8"标题辉光初始化失败（标题将不带发光）");

	// 按钮特效失败不致命，custom::button 会退回纯色圆角矩形。
	if (!button_fx::init(renderer::g_state.m_p_device, renderer::g_state.m_p_context))
		log_line(u8"按钮特效初始化失败（按钮将不带发光）");

	if (!loading_fx::init(renderer::g_state.m_p_device, renderer::g_state.m_p_context))
		log_line(u8"加载特效初始化失败（加载页将不带竖条）");

	if (!imgui_layer::init(window::hwnd, renderer::g_state.m_p_device, renderer::g_state.m_p_context, window::g_scale))
	{
		log_line(u8"ImGui 初始化失败");
		loading_fx::shutdown();
		button_fx::shutdown();
		title_glow::shutdown();
		backdrop_blur::shutdown();
		burst_background::shutdown();
		renderer::destroy();
		window::destroy();
		return 5;
	}

	task_queue::start();
	log_line(u8"初始化完成，进入主循环");

	constexpr float k_clear_color[4] = { 0.05f, 0.05f, 0.06f, 1.0f };

	LARGE_INTEGER frequency = {};
	LARGE_INTEGER previous = {};
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&previous);

	burst_background::set_pointer(0.0f, 0.0f, false);

	bool running = true;
	while (running)
	{
		MSG msg = {};
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				running = false;
				break;
			}

			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		if (!running) break;

		LARGE_INTEGER now = {};
		QueryPerformanceCounter(&now);
		const float delta_seconds = static_cast<float>(now.QuadPart - previous.QuadPart) /
			static_cast<float>(frequency.QuadPart);
		previous = now;

		float clear_color[4] = { k_clear_color[0], k_clear_color[1], k_clear_color[2], k_clear_color[3] };

		renderer::begin_frame(clear_color);

		// 1. 背景特效先铺满整个 back buffer
		burst_background::render(delta_seconds);

		// 2. 面板区域先糊掉。加载页和登录页共用这一块，所以启动第一帧也要有矩形。
		pages::layout(window::g_scale, pages::g_state);
		const ImVec4& panel = pages::g_state.m_panel;
		if (panel.z > 0.0f && panel.w > 0.0f)
			backdrop_blur::render(panel.x, panel.y, panel.z, panel.w, pages::g_state.m_panel_rounding);

		// 3. ImGui 画在模糊层上面，鼠标位置在这里同步给背景
		imgui_layer::begin();

		const ImGuiIO& io = ImGui::GetIO();
		const bool pointer_inside =
			io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f &&
			io.MousePos.x < io.DisplaySize.x && io.MousePos.y < io.DisplaySize.y;
		burst_background::set_pointer(io.MousePos.x, io.MousePos.y, pointer_inside);

		imgui_layer::draw(window::g_scale, io.MousePos.x, io.MousePos.y, pointer_inside);

		// 除控件外整个窗口都能拖动：必须在 draw() 之后、end() 之前判断，
		// 这时本帧的悬停结果已经算好。
		imgui_layer::handle_window_drag(window::hwnd);

		imgui_layer::end();

		// 4. 标题辉光：ImGui 已经把标题文字画进 back buffer 了，这里补一圈后期光。
		//    必须排在 end() 之后，否则光会被文字和面板背景盖住；
		//    加法混合只增亮，不会把文字糊掉。
		{
			ImTextureID atlas_id = ImGui::GetIO().Fonts->TexRef.GetTexID();
			title_glow::render(reinterpret_cast<ID3D11ShaderResourceView*>(
				static_cast<intptr_t>(atlas_id)));
		}

		renderer::end_frame();
	}

	log_line(u8"退出主循环，开始释放资源");
	task_queue::shutdown();

	imgui_layer::shutdown();
	loading_fx::shutdown();
	button_fx::shutdown();
	title_glow::shutdown();
	backdrop_blur::shutdown();
	burst_background::shutdown();
	renderer::destroy();
	window::destroy();

	return 0;
}
