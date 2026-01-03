// src/runtime/bud_engine.cpp
module;

#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <print>

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

	rhi_ = std::make_unique<bud::graphics::VulkanRHI>();

#ifdef _DEBUG
	bool enable_validation = true;
#else
	bool enable_validation = false;
#endif

	rhi_->init(window_->get_sdl_window(), task_scheduler_.get(), enable_validation);
}

BudEngine::~BudEngine() {
	task_scheduler_->stop();
	rhi_->cleanup();
}

void BudEngine::run() {
	// Initialize the main thread as a worker
	task_scheduler_->init_main_thread_worker();

	//
	auto last_reload_time = std::chrono::high_resolution_clock::now();

	// 2. 游戏主循环 (Game Loop)
	while (!window_->should_close()) {

		window_->poll_events();

		if (window_->is_key_pressed(bud::platform::Key::R)) {
			auto now = std::chrono::high_resolution_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reload_time);
			if (duration.count() > 500) { // 500ms 防抖
				std::println("[Engine] Reloading shaders...");
				last_reload_time = now;
				// 异步重载着色器
				rhi_.get()->reload_shaders_async();
			}
		}

		task_scheduler_->pump_main_thread_tasks();

		//perform_frame_logic();

		rhi_->draw_frame();

		FrameMark;
	}

	rhi_->wait_idle();
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
