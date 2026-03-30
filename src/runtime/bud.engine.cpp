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

		// Initialize VirtualFileSystem as early as possible and anchor root path
		virtual_file_system = std::make_unique<bud::io::VirtualFileSystem>();
		logger = std::make_unique<bud::Logger>(virtual_file_system.get()->get_root_path());
		bud::set_global_logger(logger.get());

		// Inform crash handler of project root so dumps land under <root>/tmp
		bud::platform::set_crash_dump_root(virtual_file_system.get()->get_root_path().string().c_str());
		bud::platform::install_crash_handler();

		auto flags = engine_config.is_headless ? bud::platform::WindowFlags::Hidden : bud::platform::WindowFlags::Default;
		window = bud::platform::create_window(engine_config.name, engine_config.width, engine_config.height, flags);

		task_scheduler = std::make_unique<bud::threading::TaskScheduler>();



		int initial_width = 0;
		int initial_height = 0;
		window->get_size(initial_width, initial_height);
		last_width = initial_width;
		last_height = initial_height;

		asset_manager = std::make_unique<bud::io::AssetManager>(virtual_file_system.get(), task_scheduler.get());

		rhi = bud::graphics::create_rhi(engine_config.backend);

		auto enable_validation = engine_config.enable_validation;
#if not defined(BUD_BUILD_DEBUG)
		enable_validation = false;
#endif
		rhi->init(window.get(), task_scheduler.get(), enable_validation, engine_config.inflight_frame_count, engine_config.is_headless);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& imgui_io = ImGui::GetIO();
		
		auto resolved_imgui_path = virtual_file_system->resolve_path("src/ui/config/imgui.ini");
		if (resolved_imgui_path) {
			imgui_ini_path = resolved_imgui_path->string();
			imgui_io.IniFilename = imgui_ini_path.c_str();
		} else {
			imgui_io.IniFilename = nullptr;
		}

		imgui_io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		ImGui::StyleColorsDark();
		ImGui_ImplSDL3_InitForOther(window->get_sdl_window());

		renderer = std::make_unique<bud::graphics::Renderer>(rhi.get(), asset_manager.get(), task_scheduler.get());

		render_scenes.resize(engine_config.inflight_frame_count);

		camera_sequencer = bud::scene::CameraSequencer(asset_manager.get(), virtual_file_system.get());

		camera_sequencer.load_latest();

		// Initialize the main thread as a worker
		task_scheduler->init_main_thread_worker();
	}

	BudEngine::~BudEngine() {
		camera_sequencer.flush();

		asset_manager.reset();
		renderer.reset();

		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();

		rhi->cleanup();
		rhi.reset();

		bud::set_global_logger(nullptr);
	}

	void BudEngine::run(GameLogic perform_game_logic) {
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

			// 逻辑更新
			bool logic_updated = false;
			while (accumulator >= fixed_dt) {
				if (perform_game_logic) {
					bud::threading::Counter logic_counter;
					task_scheduler->spawn("GameLogic", [&]() {
						perform_game_logic((float)fixed_dt);
						camera_sequencer.update((float)fixed_dt, scene.main_camera);
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

			// 渲染
			uint32_t render_idx = last_committed_index.load(std::memory_order_acquire);

			perform_rendering((float)frame_time, render_idx);

			FrameMark;
		}

		task_scheduler->wait_for_counter(render_task_counter);
		rhi->wait_idle();
	}

	void BudEngine::step(float fixed_dt, GameLogic perform_game_logic) {
		task_scheduler->pump_main_thread_tasks();
		handle_events();

		// Update logic once
		if (perform_game_logic) {
			bud::threading::Counter logic_counter;
			task_scheduler->spawn("GameLogic_Step", [&]() {
				perform_game_logic((float)fixed_dt);
				camera_sequencer.update((float)fixed_dt, scene.main_camera);
			}, &logic_counter);
			task_scheduler->wait_for_counter(logic_counter);
		}

		// Stage Render Data
		uint32_t current_write_index = render_inflight_index.load(std::memory_order_acquire);
		uint32_t next_write_index = (current_write_index + 1) % render_scenes.size();
		if (next_write_index == render_inflight_index.load(std::memory_order_acquire)) {
			task_scheduler->wait_for_counter(render_task_counter);
		}

		current_write_index = next_write_index;
		prepare_render_scene(current_write_index);
		last_committed_index.store(current_write_index, std::memory_order_release);

		// Render Frame immediately inline for step() determinism
		uint32_t render_idx = last_committed_index.load(std::memory_order_acquire);
		perform_rendering(fixed_dt, render_idx);

		if (engine_config.is_puppet_mode) {
			task_scheduler->wait_for_counter(render_task_counter);
			rhi->wait_idle(); // Guarantee pixel copy is mapped to RAM
		}
	}

	void BudEngine::handle_events() {
		window->poll_events();

		bool is_playing = camera_sequencer.is_playing();

		static bool was_f3_down = false;
		bool is_f3_down = bud::input::Input::get().is_key_down(bud::input::Key::F3);
		if (is_f3_down && !was_f3_down && !is_playing) {
			show_debug_stats = !show_debug_stats;
		}
		was_f3_down = is_f3_down;

		static bool was_f4_down = false;
		bool is_f4_down = bud::input::Input::get().is_key_down(bud::input::Key::F4);
		if (is_f4_down && !was_f4_down && !is_playing) {
			auto config = renderer->get_config();
			config.enable_cluster_visualization = !config.enable_cluster_visualization;
			renderer->set_config(config);
		}
		was_f4_down = is_f4_down;

		// Space: toggle playback pause
		static bool was_space_down = false;
		bool is_space_down = bud::input::Input::get().is_key_down(bud::input::Key::Space);
		if (is_space_down && !was_space_down) {
			camera_sequencer.toggle_pause();
		}
		was_space_down = is_space_down;

		// F8: toggle camera recording — ignored during playback to protect the loaded track.
		static bool was_f8_down = false;
		bool is_f8_down = bud::input::Input::get().is_key_down(bud::input::Key::F8);
		if (is_f8_down && !was_f8_down &&
		    camera_sequencer.get_state() != bud::scene::SequencerState::PLAYING)
		{
			if (camera_sequencer.get_state() == bud::scene::SequencerState::RECORDING) {
				camera_sequencer.stop_recording();
			} else {
				camera_sequencer.start_recording(scene.main_camera);
			}
		}
		was_f8_down = is_f8_down;

		// F9: toggle camera playback
		// Ctrl+F9: toggle looping camera playback
		static bool was_f9_down = false;
		bool is_f9_down  = bud::input::Input::get().is_key_down(bud::input::Key::F9);
		bool is_ctrl_down = bud::input::Input::get().is_key_down(bud::input::Key::LCtrl);
		if (is_f9_down && !was_f9_down) {
			if (camera_sequencer.get_state() == bud::scene::SequencerState::PLAYING) {
				camera_sequencer.stop_playback();
			} else {
				const bool loop = is_ctrl_down;
				camera_sequencer.start_playback(loop);
			}
		}
		was_f9_down = is_f9_down;

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

		if (!engine_config.is_puppet_mode) {
			ImGui_ImplSDL3_NewFrame();
			ImGui::NewFrame();

			if (show_debug_stats) {
				auto stats = rhi->get_stats();
				ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
				ImGui::SetNextWindowBgAlpha(0.35f);
				if (ImGui::Begin("Engine Stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {

					// Build sequencer status string for same-line FPS display
					std::string seq_status;
					const auto seq_state      = camera_sequencer.get_state();
					const auto keyframe_count = camera_sequencer.get_keyframe_count();
					if (seq_state == bud::scene::SequencerState::RECORDING) {
						seq_status = std::format("| REC [{} kf]", keyframe_count);
					} else if (seq_state == bud::scene::SequencerState::PLAYING) {
						if (camera_sequencer.is_paused()) {
							seq_status = std::format("| PAUS [{}/{}]",
								camera_sequencer.get_playback_index(), keyframe_count);
						} else {
							const char* mode = camera_sequencer.is_looping() ? "LOOP" : "PLAY";
							seq_status = std::format("| {} [{}/{}]",
								mode, camera_sequencer.get_playback_index(), keyframe_count);
						}
					} else if (keyframe_count > 0) {
						seq_status = std::format("| READY [{} kf]", keyframe_count);
					}

					bud::ui::StatsUI::render(stats, view_snapshot.delta_time, seq_status);
				}
				ImGui::End();
			}

			ImGui::Render();
			renderer->update_ui_draw_data(ImGui::GetDrawData());
		}

		// 发射渲染任务 (Fire and Forget), Pin to Worker 1 for Vulkan WSI safety
		task_scheduler->spawn_on_thread(1, "RenderTask", [this, render_scene_index, view_snapshot]() mutable {
			renderer->render(render_scenes[render_scene_index], view_snapshot);
			render_inflight_index.store(BudEngine::invalid_render_index, std::memory_order_release);
		}, &render_task_counter);
	}
} // namespace bud::engine
