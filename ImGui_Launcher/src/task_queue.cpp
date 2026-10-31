#include "pch.hpp"

#include "task_queue.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace task_queue
{
	namespace
	{
		constexpr int k_worker_count = 2;

		std::mutex g_mutex;
		std::condition_variable g_cv;
		std::deque<std::function<void()>> g_jobs;
		std::vector<std::thread> g_workers;
		bool g_started = false;
		bool g_stopping = false;

		void worker_main()
		{
			for (;;)
			{
				std::function<void()> job;
				{
					std::unique_lock<std::mutex> lock(g_mutex);
					g_cv.wait(lock, [] { return g_stopping || !g_jobs.empty(); });
					if (g_stopping && g_jobs.empty())
						return;

					job = std::move(g_jobs.front());
					g_jobs.pop_front();
				}

				job();
			}
		}
	}

	void start()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_started)
			return;

		g_stopping = false;
		g_started = true;
		g_workers.reserve(k_worker_count);
		for (int i_worker = 0; i_worker < k_worker_count; ++i_worker)
			g_workers.emplace_back(worker_main);
	}

	void push(std::function<void()> job)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_started || g_stopping)
				return;

			g_jobs.push_back(std::move(job));
		}

		g_cv.notify_one();
	}

	void shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_started)
				return;

			g_stopping = true;
		}

		g_cv.notify_all();
		for (std::thread& worker : g_workers)
		{
			if (worker.joinable())
				worker.join();
		}

		g_workers.clear();
		g_jobs.clear();
		g_started = false;
		g_stopping = false;
	}
}
