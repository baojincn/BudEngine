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

	rhi_ = bud::graphics::create_rhi(bud::graphics::Backend::Vulkan);

#ifdef _DEBUG
	bool enable_validation = true;
#else
	bool enable_validation = false;
#endif

	rhi_->init(window_.get(), task_scheduler_.get(), enable_validation);

	camera_ = bud::graphics::Camera(bud::math::vec3(10.0f, 5.0f, 0.0f));
	camera_.movement_speed = 100.0f;

	bud::graphics::RenderConfig config;

	config.shadowMapSize = 4096;

	config.lightPos = { 500.0f, 1000.0f, 10.0f };
	config.lightColor = { 1.0f, 1.0f, 1.0f };
	config.lightIntensity = 3.0f;

	config.shadowBiasConstant = 2.25f;
	config.shadowBiasSlope = 4.75f;
	config.shadowOrthoSize = 2000.0f;
	config.shadowNear = 1.0f;
	config.shadowFar = 5000.0f;

	rhi_->load_model_async("data/cryteksponza/sponza.obj");

	rhi_->set_config(config);
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

	//
	auto last_frame_time = std::chrono::high_resolution_clock::now();

	// 2. 游戏主循环 (Game Loop)
	while (!window_->should_close()) {
		// Priority to process window events
		handle_events();

		auto current_time = std::chrono::high_resolution_clock::now();
		auto delta_time = std::chrono::duration<float, std::chrono::seconds::period>(current_time - last_frame_time).count();
		last_frame_time = current_time;

		static float timer = 0.0f;
		timer += delta_time;
		static int fps = 0;
		fps++;

		const std::string base_title = "Bud Engine - Triangle Sample";
		if (timer >= 1.0f) {
			auto new_title = std::format("{} FPS: {}", base_title, fps);
			window_->set_title(new_title.c_str());
			timer = 0.0f;
			fps = 0;
		}

		if (window_->is_key_pressed(bud::platform::Key::R)) {
			auto now = std::chrono::high_resolution_clock::now();
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reload_time);
			if (duration.count() > 500) { // 500ms 防抖
				std::println("[Engine] Reloading shaders...");
				last_reload_time = now;
				rhi_.get()->reload_shaders_async();
			}
		}

		task_scheduler_->pump_main_thread_tasks();

		perform_frame_logic(delta_time);

		perform_rendering();

		FrameMark;
	}

	rhi_->wait_idle();
}

void BudEngine::handle_events() {
	window_->poll_events();
}

void BudEngine::perform_frame_logic(float delta_time) {

	// 键盘移动 (WASD)
	if (window_->is_key_pressed(bud::platform::Key::W))
		camera_.process_keyboard(0, delta_time); // 0 = FORWARD
	if (window_->is_key_pressed(bud::platform::Key::S))
		camera_.process_keyboard(1, delta_time); // 1 = BACKWARD
	if (window_->is_key_pressed(bud::platform::Key::A))
		camera_.process_keyboard(2, delta_time); // 2 = LEFT
	if (window_->is_key_pressed(bud::platform::Key::D))
		camera_.process_keyboard(3, delta_time); // 3 = RIGHT

	// 鼠标滚轮缩放
	float dx, dy;
	window_->get_mouse_delta(dx, dy);


	// Look Around
	if (window_->is_mouse_button_down(bud::platform::MouseButton::Left)) {
		if (dx != 0.0f || dy != 0.0f) {
			camera_.process_mouse_movement(dx, dy);
		}
	}
	else // Zoom
		if (window_->is_mouse_button_down(bud::platform::MouseButton::Right)) {
			if (dy != 0.0f) {
				camera_.process_mouse_drag_zoom(dy);
			}
		}
}

void BudEngine::perform_rendering() {
	int width, height;
	window_->get_size(width, height);

	if (height == 0)
		height = 1; // Avoid division by zero

	aspect_ratio_ = static_cast<float>(width) / static_cast<float>(height);

	auto proj = bud::math::perspective_vk(camera_.zoom, aspect_ratio_, near_plane_, far_plane_);

	rhi_->draw_frame(camera_.get_view_matrix(), proj);
}
