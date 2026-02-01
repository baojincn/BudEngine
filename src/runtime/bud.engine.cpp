

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

#include "src/runtime/bud.engine.hpp"

#include <print>

#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"

namespace bud::engine {

	BudEngine::BudEngine(const std::string& window_title, int width, int height) {
		window = bud::platform::create_window(window_title, width, height);
		task_scheduler = std::make_unique<bud::threading::TaskScheduler>();

		asset_manager = std::make_unique<bud::io::AssetManager>(task_scheduler.get());

		rhi = bud::graphics::create_rhi(bud::graphics::Backend::Vulkan);

#ifdef _DEBUG
		bool enable_validation = true;
#else
		bool enable_validation = false;
#endif
		rhi->init(window.get(), task_scheduler.get(), enable_validation);

		renderer = std::make_unique<bud::graphics::Renderer>(rhi.get(), asset_manager.get());

		scene.main_camera = bud::scene::Camera(bud::math::vec3(0.0f, 100.0f, 0.0f));
		scene.main_camera.movement_speed = 50.0f;
	}

	BudEngine::~BudEngine() {
		task_scheduler->stop();
		renderer.reset();
		rhi->cleanup();
	}

	void BudEngine::run(TickCallback tick) {
		// Initialize the main thread as a worker
		task_scheduler->init_main_thread_worker();

		auto& input = bud::input::Input::get();

		using Clock = std::chrono::high_resolution_clock;
		auto start_time = Clock::now();
		auto last_frame_time = start_time;

		std::println("[Engine] Entering Main Loop...");

		while (!window->should_close()) {
			handle_events();

			task_scheduler->pump_main_thread_tasks();

			auto current_time = Clock::now();
			auto delta_time = std::chrono::duration<float>(current_time - last_frame_time).count();
			last_frame_time = current_time;

			// 计算 FPS
			static auto timer = 0.0f;
			static auto fps = 0;
			timer += delta_time;
			fps++;
			if (timer >= 1.0f) {
				auto title = std::format("Bud Engine - FPS: {}", fps);
				window->set_title(title.c_str());
				timer = 0.0f;
				fps = 0;
			}

			if (tick) {
				tick(delta_time);
			}

			if (input.is_key_down(bud::input::Key::R)) {
				// TODO: Reload shaders logic test
			}


			perform_frame_logic(delta_time);

			perform_rendering(delta_time);

			FrameMark;
		}

		rhi->wait_idle();
	}

	void BudEngine::handle_events() {
		window->poll_events();
	}

	void BudEngine::perform_frame_logic(float delta_time) {
		auto& input = bud::input::Input::get();
		auto& cam = scene.main_camera;

		if (input.is_key_down(bud::input::Key::W)) cam.process_keyboard(0, delta_time);
		if (input.is_key_down(bud::input::Key::S)) cam.process_keyboard(1, delta_time);
		if (input.is_key_down(bud::input::Key::A)) cam.process_keyboard(2, delta_time);
		if (input.is_key_down(bud::input::Key::D)) cam.process_keyboard(3, delta_time);



		float dx, dy;
		input.get_mouse_delta(dx, dy);

		if (input.is_mouse_button_down(bud::input::MouseButton::Left)) {
			if (dx != 0.0f || dy != 0.0f)
				cam.process_mouse_movement(dx, dy);
		}
		else if (input.is_mouse_button_down(bud::input::MouseButton::Right)) {
			if (dy != 0.0f)
				cam.process_mouse_drag_zoom(dy);
		}
	}

	void BudEngine::perform_rendering(float delta_time) {
		int width, height;
		window->get_size(width, height);
		if (height == 0)
			height = 1;

		// 更新 SceneView
		bud::graphics::SceneView view;
		view.viewport_width = static_cast<float>(width);
		view.viewport_height = static_cast<float>(height);

		// 计算总运行时间
		using Clock = std::chrono::high_resolution_clock;
		static auto start_time = Clock::now();
		view.time = std::chrono::duration<float>(Clock::now() - start_time).count();
		view.delta_time = delta_time;

		auto aspect = view.viewport_width / view.viewport_height;
		view.view_matrix = scene.main_camera.get_view_matrix();
		view.proj_matrix = bud::math::perspective_vk(scene.main_camera.zoom, aspect, near_plane, far_plane);
		view.camera_position = scene.main_camera.position;
		view.near_plane = near_plane;
		view.far_plane = far_plane;

		auto& config = renderer->get_config();
		view.light_dir = bud::math::normalize(config.directional_light_position);
		view.light_color = config.directional_light_color;

		view.update_matrices();

		renderer->render(scene, view);
	}
} // namespace bud::engine
