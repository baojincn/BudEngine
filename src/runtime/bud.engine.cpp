#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <cmath>
#include <print>

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#else
#define ZoneScoped
#define FrameMark
#endif

#include "src/io/bud.io.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/ui/bud.stats.ui.hpp"
#include "src/core/bud.logger.hpp"
#include "src/platform/crash_handler.hpp"

#include "src/runtime/bud.engine.hpp"
#include "src/graphics/vulkan/bud.graphics.vulkan.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace bud::engine {

	BudEngine::BudEngine(const bud::graphics::EngineConfig config) : engine_config(config) {

		window = bud::platform::create_window(engine_config.name, engine_config.width, engine_config.height);
		// Install platform crash handler as early as possible so crashes during
		// initialization produce minidumps for post-mortem analysis.
		bud::platform::install_crash_handler();
		task_scheduler = std::make_unique<bud::threading::TaskScheduler>();

		// Create engine-owned logger and inject TaskScheduler for async log writes.
		logger = std::make_unique<bud::Logger>(task_scheduler.get());


		int initial_width = 0;
		int initial_height = 0;
		window->get_size(initial_width, initial_height);
		last_width = initial_width;
		last_height = initial_height;

		asset_manager = std::make_unique<bud::io::AssetManager>(task_scheduler.get());

		// TaskScheduler already injected via logger constructor above.

		rhi = bud::graphics::create_rhi(engine_config.backend);

		auto enable_validation = engine_config.enable_validation;
#if not defined(BUD_BUILD_DEBUG)
		enable_validation = false;
#endif
		rhi->init(window.get(), task_scheduler.get(), enable_validation, engine_config.inflight_frame_count);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& imgui_io = ImGui::GetIO();
		
		auto resolved_imgui_path = bud::io::FileSystem::resolve_path("src/ui/config/imgui.ini");
		if (resolved_imgui_path) {
			imgui_ini_path = resolved_imgui_path->string();
			imgui_io.IniFilename = imgui_ini_path.c_str();
		} else {
			imgui_io.IniFilename = nullptr; // Don't save if path is invalid
		}

		imgui_io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		ImGui::StyleColorsDark();
		ImGui_ImplSDL3_InitForOther(window->get_sdl_window());

		renderer = std::make_unique<bud::graphics::Renderer>(rhi.get(), asset_manager.get(), task_scheduler.get());

		render_scenes.resize(engine_config.inflight_frame_count);
	}

	BudEngine::~BudEngine() {
		asset_manager.reset();
		renderer.reset();

		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();

		rhi->cleanup();
		rhi.reset();
	}

	void BudEngine::run(GameLogic perform_game_logic) {
		// Initialize the main thread as a worker
		task_scheduler->init_main_thread_worker();

		const double fixed_dt = renderer->get_config().fixed_logic_timestep;

		using Clock = std::chrono::high_resolution_clock;
		auto last_time = Clock::now();

		while (!window->should_close()) {
			task_scheduler->pump_main_thread_tasks();
			handle_events();


			auto now = Clock::now();
			double frame_time = std::chrono::duration<double>(now - last_time).count();
			last_time = now;

			// 防止螺旋死亡
			if (frame_time > 0.25)
				frame_time = 0.25;

			accumulator += frame_time;

			// 阶段 A: 逻辑更新
			bool logic_updated = false;
			while (accumulator >= fixed_dt) {
				if (perform_game_logic) {
					bud::threading::Counter logic_counter;
					task_scheduler->spawn("GameLogic", [&]() {
						perform_game_logic((float)fixed_dt);
					}, &logic_counter);
					task_scheduler->wait_for_counter(logic_counter);
				}

				accumulator -= fixed_dt;
				logic_updated = true;
			}

			if (logic_updated) {
				uint32_t next_write_index = (current_write_index + 1) % render_scenes.size();
				if (next_write_index == render_inflight_index.load(std::memory_order_acquire)) {
					task_scheduler->wait_for_counter(render_task_counter);
				}

				current_write_index = next_write_index;
				prepare_render_scene(current_write_index);
				last_committed_index.store(current_write_index, std::memory_order_release);
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

		static bool was_f3_down = false;
		bool is_f3_down = bud::input::Input::get().is_key_down(bud::input::Key::F3);
		if (is_f3_down && !was_f3_down) {
			show_debug_stats = !show_debug_stats;
		}
		was_f3_down = is_f3_down;

		static bool was_f4_down = false;
		bool is_f4_down = bud::input::Input::get().is_key_down(bud::input::Key::F4);
		if (is_f4_down && !was_f4_down) {
			auto config = renderer->get_config();
			config.enable_cluster_visualization = !config.enable_cluster_visualization;
			renderer->set_config(config);
		}
		was_f4_down = is_f4_down;

		int width = 0;
		int height = 0;
		window->get_size_in_pixels(width, height);

		if (width != last_width || height != last_height || rhi->is_swapchain_out_of_date()) {
			last_width = width;
			last_height = height;

			if (width > 0 && height > 0) {
				task_scheduler->wait_for_counter(render_task_counter);
				rhi->resize_swapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			}
		}
	}

	void BudEngine::extract_render_scene_data(bud::graphics::RenderScene& render_scene) {
		ZoneScoped;
		auto& logic_entities = scene.entities;
		auto submesh_bounds_all = renderer->get_submesh_bounds_snapshot();

		// Calculate total submesh count for capacity reservation
		size_t total_submesh_count = logic_entities.size();

		constexpr size_t buffering_size = 256;
		render_scene.reset(total_submesh_count + buffering_size);

		auto mesh_bounds = renderer->get_mesh_bounds_snapshot();

		bud::threading::Counter extract_scene_counter;

		// 设定分块大小 (Granularity)。太小会导致调度开销，太大导致负载不均。
		// 经验值：64 ~ 256 个实体一个 Job。
		constexpr size_t CHUNK_SIZE = 128;

		task_scheduler->ParallelFor(logic_entities.size(), CHUNK_SIZE,
			[&](size_t start, size_t end_exclusive) {
				for (size_t i = start; i < end_exclusive; ++i) {
					const auto& entity = logic_entities[i];
					if (entity.mesh_index == bud::asset::INVALID_INDEX)
						continue;

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
						bud::asset::INVALID_INDEX, // Let renderer explode
						entity.material_index,
						entity.is_static
					);
				}
			},
			&extract_scene_counter
		);

		task_scheduler->wait_for_counter(extract_scene_counter);

		FrameMark;
	}

	void BudEngine::prepare_render_scene(uint32_t render_scene_index) {
		ZoneScoped;
		
		extract_render_scene_data(render_scenes[render_scene_index]);
		render_scenes[render_scene_index].build_culling_lbvh_parallel(task_scheduler.get());

		FrameMark;
	}

	void BudEngine::perform_rendering(float delta_time, uint32_t render_scene_index) {
		// Wait for previous frame's render task to complete
		task_scheduler->wait_for_counter(render_task_counter);

		int width, height;
		window->get_size(width, height);

		// If the window is minimized or invisible, do not attempt to render.
		// Hammering vkAcquireNextImageKHR on a 0x0 un-resized window causes AMD/NVIDIA driver TDRs (Hang/BSOD).
		if (width == 0 || height == 0) {
			render_inflight_index.store(BudEngine::invalid_render_index, std::memory_order_release);
			std::this_thread::sleep_for(std::chrono::nanoseconds(1)); // prevent 100% CPU spin
			return;
		}

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
		auto render_config = renderer->get_config();
		view_snapshot.view_matrix = scene.main_camera.get_view_matrix();
		if (render_config.reversed_z) {
			view_snapshot.proj_matrix = bud::math::perspective_vk_reversed(scene.main_camera.zoom, aspect, near_plane, far_plane);
		} else {
			view_snapshot.proj_matrix = bud::math::perspective_vk(scene.main_camera.zoom, aspect, near_plane, far_plane);
		}
		view_snapshot.camera_position = scene.main_camera.position;
		view_snapshot.near_plane = near_plane;
		view_snapshot.far_plane = far_plane;

		view_snapshot.light_dir = bud::math::normalize(scene.directional_light.direction);
		view_snapshot.light_color = scene.directional_light.color;
		view_snapshot.light_intensity = scene.directional_light.intensity;
		view_snapshot.ambient_strength = scene.ambient_strength;

		view_snapshot.show_debug_stats = show_debug_stats;

		view_snapshot.update_matrices();

		render_inflight_index.store(render_scene_index, std::memory_order_release);

		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		if (show_debug_stats) {
			auto stats = rhi->get_stats();
			ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
			ImGui::SetNextWindowBgAlpha(0.35f);
			if (ImGui::Begin("Engine Stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
				bud::ui::StatsUI::render(stats, view_snapshot.delta_time);
			}
			ImGui::End();
		}

		ImGui::Render();
		renderer->update_ui_draw_data(ImGui::GetDrawData());

		// 发射渲染任务 (Fire and Forget), Pin to Worker 1 for Vulkan WSI safety
		task_scheduler->spawn_on_thread(1, "RenderTask", [this, render_scene_index, view_snapshot]() mutable {
			renderer->render(render_scenes[render_scene_index], view_snapshot);
			render_inflight_index.store(BudEngine::invalid_render_index, std::memory_order_release);
		}, &render_task_counter);
	}
} // namespace bud::engine
