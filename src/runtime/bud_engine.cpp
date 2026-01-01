// src/runtime/bud_engine.cpp
module;

#include <string>
#include <memory>
#include <thread>
#include <chrono>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#else
#define ZoneScoped
#define FrameMark
#endif

module bud.engine;

using namespace bud::engine;

BudEngine::BudEngine(const std::string& window_title, int width, int height) {
	window_ = bud::platform::create_window(window_title, width, height);
	task_scheduler_ = std::make_unique<bud::threading::TaskScheduler>();
}

BudEngine::~BudEngine() {
	task_scheduler_->stop();
}

void BudEngine::run() {
	// Initialize the main thread as a worker
	task_scheduler_->init_main_thread_worker();

	// 2. 游戏主循环 (Game Loop)
	while (!window_->should_close()) {

		window_->poll_events();

		perform_frame_logic();

		// --- C. 协助执行任务 ---
		task_scheduler_->pump_main_thread_tasks();

		FrameMark;

		// Avoid busy waiting, 100% CPU usage
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void BudEngine::perform_frame_logic() {
	for (int i = 0; i < 100; ++i) {
		task_scheduler_->spawn([i] {
			// Simulate some work
			int x = 0;
			for (int j = 0; j < 10000; j++)
				x += j;
		},
		&frame_counter_);
	}

	// Synchronize logic and processing of window events
	task_scheduler_->wait_for_counter(frame_counter_, [this]() {
		window_->poll_events();
	});

	// Render frame

	// Compile Render Graphh

	// Execute Render Graph

	// Commit

	// Present

	// Reset frame counter for next frame
}
