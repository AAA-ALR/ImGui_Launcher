#pragma once

#include <functional>

// 很小的工作线程池。UI 线程只负责 push，真正的登录请求在这里跑。
// 页面切换不要在任务里直接改 page_state，走 pages::show_loading / show_page。

namespace task_queue
{
	void start();
	void push(std::function<void()> job);
	void shutdown();
}
