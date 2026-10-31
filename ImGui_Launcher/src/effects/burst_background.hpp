#pragma once

#include "pch.hpp"

// Prismatic Burst 背景特效（HLSL 移植版）
//
// 用法：
//   burst_background::init(device, context, width, height);   // 只在启动时调一次
//   burst_background::trigger();                              // 想触发就调一次
//   burst_background::render();                               // 每帧一次，画在 ImGui 之前
//
// 窗口大小是启动时算好的固定值，因此渲染目标只分配一次，不再随窗口变化。
//
// 所有参数都在 config() 返回的 settings 上就地修改，下一帧生效，
// 不需要重新 init；只有 colors 变化会触发一次渐变纹理重传。

namespace burst_background
{
	// 背景特效的全部运行期参数。
	// 这些值每帧都会读一次，改完立刻生效，不影响已经分配好的渲染目标。
	struct settings
	{
		// ---- 造型 ----

		// 整体亮度增益。1.0 = 原始亮度，2.0 是网页版默认值。
		// 太大时高光会顶到白色，靠 composite 的 knee 压回来。
		float intensity = 2.0f;

		// 旋转与颜色流动的速度倍率。0 = 完全静止，1.0 偏快。
		float speed = 0.5f;

		// 视距缩放。>1 拉近（光心变大、光束变粗），<1 推远。
		float zoom = 1.0f;

		// 单根光束的粗细倍率。0.2 是细线，2.5 附近开始互相重叠。
		float ray_width = 1.0f;

		// 光线向画面中心弯曲的程度，单位近似为像素当量。
		// 0 = 直线发散，数值越大越像被引力吸住。
		float distort = 0.0f;

		// 沿半径方向的螺旋扭曲。正负决定旋向，±3 已经非常夸张。
		float twist = 0.0f;

		// 颜色沿光束流动的速度。0 = 静态配色，1.2 左右开始明显游动。
		float flow = 0.0f;

		// 中心暗核的大小。1.0 = 网页版默认，0 = 中心不压暗。
		float center_fade = 1.0f;

		// 胶片颗粒强度。0 = 干净，1.0 = 明显噪点，网页版默认 0.8。
		float grain = 0.8f;

		// bloom 光晕强度。0 关闭辉光（画面更锐利但更暗），1.0 以上开始发雾。
		float glow = 0.55f;

		// 四边淡出。0 = 画满整个窗口，1.0 = 边缘完全融进背景色。
		float fade = 0.0f;

		// 光束数量。0 = 按噪声自然发散（网页版行为），
		// >0 = 均匀分成固定根数，画面更规整。
		int ray_count = 0;

		// ---- 渲染质量 ----

		// 场景离屏目标相对窗口分辨率的比例，范围 0.2 ~ 1.0。
		// 0.7 是画质与性能的平衡点；调低会明显变糊，但帧率上升。
		// 只在 init 时读取一次，运行期修改需要重新 init 才生效。
		float resolution_scale = 0.7f;

		// 启动时从中心点亮。false = 一上来就是满强度。
		bool intro = true;

		// ---- 指针交互 ----

		// 是否响应鼠标位置（光束朝指针聚拢）。
		bool mouse_interaction = true;

		// 指针聚拢的强度。0 = 无影响，1.0 = 强烈偏向光标。
		float mouse_strength = 0.5f;

		// 指针靠近后亮度衰减的阻尼，越大越跟手。
		float hover_dampness = 0.25f;

		// 总开关：false 时 trigger() 直接返回，光波事件被完全忽略。
		// 想彻底关掉点击/登录触发的效果就设它，不需要改调用方。
		bool click_burst = true;

		// 渐变配色，RGB 取值范围 0~1，按顺序沿光束采样。
		// 留空 = 使用全光谱彩虹。修改后会重新上传一张 1×N 的渐变纹理。
		std::vector<std::array<float, 3>> colors;
	};

	// 创建渲染目标与管线状态，只在启动时调用一次。
	// width / height 传入窗口客户区尺寸，内部按 resolution_scale 建离屏目标。
	bool init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);

	// 释放全部 D3D 资源。必须在设备销毁之前调用。
	void shutdown();

	// 触发一次光波爆发。可以随时调用（例如登录成功之后）。
	// 最多同时存在 4 道波，超出时最早的一道被顶掉。
	// click_burst = false 时这个调用是空操作。
	void trigger();

	// 立即停止所有光波并重置动画相位
	void reset();

	// 每帧渲染一次，必须在 ImGui 绘制之前调用
	void render(float delta_seconds);

	// 鼠标位置（客户区像素，左上原点），用于指针聚焦效果
	void set_pointer(float x, float y, bool inside);

	// 拿到参数结构体，直接改字段即可
	settings& config();

	// 当前是否有光波在活动
	bool is_bursting();
}
