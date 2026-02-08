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
#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"

namespace bud::engine {

	BudEngine::BudEngine(const bud::graphics::EngineConfig config) : engine_config(config) {

		window = bud::platform::create_window(engine_config.name, engine_config.width, engine_config.height);
		task_scheduler = std::make_unique<bud::threading::TaskScheduler>();

		asset_manager = std::make_unique<bud::io::AssetManager>(task_scheduler.get());

		rhi = bud::graphics::create_rhi(engine_config.backend);

		auto enable_validation = engine_config.enable_validation;
		rhi->init(window.get(), task_scheduler.get(), enable_validation, engine_config.inflight_frame_count);

		renderer = std::make_unique<bud::graphics::Renderer>(rhi.get(), asset_manager.get(), task_scheduler.get());

		render_scenes.resize(engine_config.inflight_frame_count);

		scene.main_camera = bud::scene::Camera(bud::math::vec3(0.0f, 100.0f, 0.0f));
		scene.main_camera.movement_speed = 70.0f;
	}

	BudEngine::~BudEngine() {
		task_scheduler->stop();
		asset_manager.reset();
		renderer.reset();
		rhi->cleanup();
		rhi.reset();
	}

	void BudEngine::run(GameLogic perform_game_logic) {
		// Initialize the main thread as a worker
		task_scheduler->init_main_thread_worker();

		// 获取配置的固定步长
		const double fixed_dt = renderer->get_config().fixed_logic_timestep;

		using Clock = std::chrono::high_resolution_clock;
		auto last_time = Clock::now();

		while (!window->should_close()) {
			task_scheduler->pump_main_thread_tasks();
			handle_events();


			auto now = Clock::now();
			double frame_time = std::chrono::duration<double>(now - last_time).count();
			last_time = now;

			// 计算 FPS
			static auto timer = 0.0f;
			static auto fps = 0;
			timer += frame_time;
			fps++;
			if (timer >= 1.0f) {
				auto title = std::format("{} - FPS: {}", engine_config.name, fps);
				window->set_title(title.c_str());
				timer = 0.0f;
				fps = 0;
			}

			// 防止螺旋死亡
			if (frame_time > 0.25)
				frame_time = 0.25;

			accumulator += frame_time;

			// 阶段 A: 逻辑更新
			while (accumulator >= fixed_dt) {
				uint32_t next_write_index = (current_write_index + 1) % render_scenes.size();

				if (next_write_index == render_inflight_index.load(std::memory_order_acquire)) {
					task_scheduler->wait_for_counter(render_task_counter);
				}

				current_write_index = next_write_index;

				if (perform_game_logic) {
					bud::threading::Counter logic_counter;

					task_scheduler->spawn("GameLogic", [&]() {
						perform_game_logic((float)fixed_dt);
					}, &logic_counter);

					task_scheduler->wait_for_counter(logic_counter);
				}

				extract_scene_data(render_scenes[current_write_index]);

				// 提交这一帧
				last_committed_index.store(current_write_index, std::memory_order_release);

				accumulator -= fixed_dt;
			}

			// 阶段 B: 渲染
			uint32_t render_idx = last_committed_index.load(std::memory_order_acquire);

			perform_rendering((float)frame_time, render_idx);

			FrameMark;
		}

		// 等待所有渲染任务完成
		task_scheduler->wait_for_counter(render_task_counter);
		rhi->wait_idle();
	}

	void BudEngine::handle_events() {
		window->poll_events();
	}

	void BudEngine::extract_scene_data(bud::graphics::RenderScene& render_scene) {
		auto& logic_entities = scene.entities;
		size_t total_logic_count = logic_entities.size();

		constexpr size_t buffering_size = 128; // 预留一些余量，避免频繁越界

		render_scene.reset(total_logic_count + buffering_size);

		auto mesh_bounds = renderer->get_mesh_bounds_snapshot();

		bud::threading::Counter extract_scene_counter;

		// 设定分块大小 (Granularity)。太小会导致调度开销，太大导致负载不均。
		// 经验值：64 ~ 256 个实体一个 Job。
		constexpr size_t CHUNK_SIZE = 128;

		task_scheduler->ParallelFor(total_logic_count, CHUNK_SIZE,
			[&](size_t start, size_t end) {
				for (size_t i = start; i <= end; ++i) {
					const auto& entity = logic_entities[i];

					if (entity.mesh_index >= mesh_bounds.size()) [[unlikely]] {
						continue;
					}

					if (!entity.is_active)
						continue; 

					const auto& world_matrix = entity.transform;

					const auto& local_aabb = mesh_bounds[entity.mesh_index];

					auto world_aabb = local_aabb.transform(world_matrix);

					render_scene.add_instance(
						world_matrix,
						world_aabb,
						entity.mesh_index,
						entity.material_index,
						entity.is_static
					);

				}
			},
			&extract_scene_counter
		);

		task_scheduler->wait_for_counter(extract_scene_counter);
	}

	void BudEngine::sync_game_to_rendering(uint32_t render_scene_index) {
		ZoneScoped;
		
		extract_scene_data(render_scenes[render_scene_index]);

		FrameMark;
	}

	void BudEngine::perform_rendering(float delta_time, uint32_t render_scene_index) {
		// Wait for previous frame's render task to complete
		task_scheduler->wait_for_counter(render_task_counter);

		int width, height;
		window->get_size(width, height);
		if (height == 0)
			height = 1;

		// 更新 SceneView
		bud::graphics::SceneView view_snapshot;
		view_snapshot.viewport_width = static_cast<float>(width);
		view_snapshot.viewport_height = static_cast<float>(height);

		// 计算总运行时间
		using Clock = std::chrono::high_resolution_clock;
		static auto start_time = Clock::now();
		view_snapshot.time = std::chrono::duration<float>(Clock::now() - start_time).count();
		view_snapshot.delta_time = delta_time;

		auto aspect = view_snapshot.viewport_width / view_snapshot.viewport_height;
		view_snapshot.view_matrix = scene.main_camera.get_view_matrix();
		view_snapshot.proj_matrix = bud::math::perspective_vk(scene.main_camera.zoom, aspect, near_plane, far_plane);
		view_snapshot.camera_position = scene.main_camera.position;
		view_snapshot.near_plane = near_plane;
		view_snapshot.far_plane = far_plane;

		auto& config = renderer->get_config();
		view_snapshot.light_dir = bud::math::normalize(config.directional_light_position);
		view_snapshot.light_color = config.directional_light_color;

		view_snapshot.update_matrices();

		render_inflight_index.store(render_scene_index, std::memory_order_release);

		// 发射渲染任务 (Fire and Forget)
		task_scheduler->spawn("RenderTask", [this, render_scene_index, view_snapshot]() mutable {
			renderer->render(render_scenes[render_scene_index], view_snapshot);
			render_inflight_index.store(BudEngine::invalid_render_index, std::memory_order_release);
		}, &render_task_counter);
	}
} // namespace bud::engine
