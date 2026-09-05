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
#include "src/runtime/bud.scene.builder.hpp"
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

		task_scheduler = std::make_unique<bud::threading::TaskScheduler>();
		input_manager = std::make_unique<bud::input::InputManager>();

		auto flags = engine_config.is_headless ? bud::platform::WindowFlags::Hidden : bud::platform::WindowFlags::Default;
		window = bud::platform::create_window(engine_config.name, engine_config.width, engine_config.height, flags);

		int initial_width = 0;
		int initial_height = 0;
		window->get_size(initial_width, initial_height);
		last_width = initial_width;
		last_height = initial_height;

		asset_manager = std::make_unique<bud::io::AssetManager>(virtual_file_system.get(), task_scheduler.get());

		rhi = bud::graphics::create_rhi(engine_config.backend);

		auto enable_validation = engine_config.enable_validation;
#if not defined(_DEBUG) && not defined(BUD_BUILD_DEBUG)
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

		streaming_manager = std::make_unique<bud::streaming::StreamingManager>(
			asset_manager.get(),
			&renderer->get_gpu_scene(),
			renderer.get(),
			rhi.get()
		);
		renderer->set_streaming_manager(streaming_manager.get());

		render_scenes.resize(engine_config.inflight_frame_count);

		camera_sequencer = bud::scene::CameraSequencer(asset_manager.get(), virtual_file_system.get());

		camera_sequencer.load_latest();

		// Initialize the main thread as a worker
		task_scheduler->init_main_thread_worker();

		// Register default action bindings
		input_manager->bind_key("ToggleDebug", bud::input::Key::F3);
		input_manager->bind_key("ToggleClusterVis", bud::input::Key::F4);
		input_manager->bind_key("ToggleWireframe", bud::input::Key::F5);
		input_manager->bind_key("ToggleDebugCascades", bud::input::Key::F6);
		input_manager->bind_key("TogglePause", bud::input::Key::Space);
		input_manager->bind_key("ToggleRecord", bud::input::Key::F8);
		input_manager->bind_key("TogglePlayback", bud::input::Key::F9);

		// Occluder adjustment via keyboard +/-
		input_manager->bind_key("OccluderDecrease", bud::input::Key::Minus);
		input_manager->bind_key("OccluderIncrease", bud::input::Key::Plus);

		// Register action callbacks (callbacks run on rising edge detected by InputManager::update())
		input_manager->register_action_callback("ToggleDebug", [this]() {
			if (!camera_sequencer.is_playing()) {
				show_debug_stats = !show_debug_stats;
			}
		});

            // Adjust heuristic occluder fraction with + / - keys
		input_manager->register_action_callback("OccluderDecrease", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto cfg = renderer->get_config();
                    cfg.heuristic_occluder_fraction = std::clamp(cfg.heuristic_occluder_fraction - 0.01f, 0.0f, 1.0f);
				renderer->set_config(cfg);
			}
		});

		input_manager->register_action_callback("OccluderIncrease", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto cfg = renderer->get_config();
                    cfg.heuristic_occluder_fraction = std::clamp(cfg.heuristic_occluder_fraction + 0.01f, 0.0f, 1.0f);
				renderer->set_config(cfg);
			}
		});

		input_manager->register_action_callback("ToggleClusterVis", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto config = renderer->get_config();
				config.enable_cluster_visualization = !config.enable_cluster_visualization;
				renderer->set_config(config);
			}
		});

		input_manager->register_action_callback("ToggleWireframe", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto config = renderer->get_config();
				config.enable_wireframe = !config.enable_wireframe;
				renderer->set_config(config);
			}
		});

		input_manager->register_action_callback("ToggleDebugCascades", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto config = renderer->get_config();
				config.debug_cascades = !config.debug_cascades;
				renderer->set_config(config);
				bud::print("[CSM] Debug cascades: {}", config.debug_cascades ? "ON" : "OFF");
			}
		});

		input_manager->register_action_callback("TogglePause", [this]() {
			camera_sequencer.toggle_pause();
		});

		input_manager->register_action_callback("ToggleRecord", [this]() {
			if (camera_sequencer.get_state() != bud::scene::SequencerState::PLAYING) {
				if (camera_sequencer.get_state() == bud::scene::SequencerState::RECORDING) {
					camera_sequencer.stop_recording();
				} else {
					camera_sequencer.start_recording(scene.main_camera);
				}
			}
		});

		input_manager->register_action_callback("TogglePlayback", [this]() {
			bool is_ctrl_down = input_manager->is_key_down(bud::input::Key::LCtrl);
			if (camera_sequencer.get_state() == bud::scene::SequencerState::PLAYING) {
				camera_sequencer.stop_playback();
			} else {
				const bool loop = is_ctrl_down;
				camera_sequencer.start_playback(loop);
			}
		});
	}

	BudEngine::~BudEngine() {
		camera_sequencer.flush();

		if (task_scheduler) {
			task_scheduler->wait_for_counter(render_task_counter);
			task_scheduler->pump_main_thread_tasks();
		}

		streaming_manager.reset();
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
						scene.main_camera.update((float)fixed_dt);
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
				scene.main_camera.update((float)fixed_dt);
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

		// Update input manager (sample current frame keys/mouse)
		if (input_manager) input_manager->update();

    	// input actions handled via InputManager callbacks registered in the constructor

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
						entity.is_static,
						entity.root_group_index,
						entity.base_virtual_page,
						entity.is_cast_shadow,
						entity.is_receive_shadow
					);
				}
			},
			&extract_scene_counter
		);

		task_scheduler->wait_for_counter(extract_scene_counter);

		// --- Backdrop discovery for full-scene shadow casters ---------------------
		// Feeding the cascades the whole scene (RenderConfig::shadow_full_scene_casters)
		// is the correct CSM model: an object outside the primary camera frustum must
		// still be rasterized into the cascade it falls in, otherwise shadows break the
		// moment the player looks up or down. What that surfaces is authored backdrop
		// geometry - a thin lid lying over the whole model - which then blocks the sun
		// from everything. Those objects need "is_cast_shadow": false in the scene file.
		// The runtime instance index is NOT a usable key for that: add_instance() above
		// is driven by a ParallelFor through an atomic counter, so instance order is not
		// stable. Report by asset_path instead, which is exactly what the scene file
		// stores. Re-printing is suppressed by a signature of the current candidate set.
		{
			std::vector<bud::math::AABB> boxes;
			std::vector<size_t> box_entity;                   // boxes[k] <- logic_entities[box_entity[k]]
			std::vector<std::pair<float, size_t>> by_footprint;   // (x*z footprint, box slot k)
			boxes.reserve(logic_entities.size());
			box_entity.reserve(logic_entities.size());
			by_footprint.reserve(logic_entities.size());
			bud::math::vec3 bmin(1e30f, 1e30f, 1e30f), bmax(-1e30f, -1e30f, -1e30f);

			for (size_t i = 0; i < logic_entities.size(); ++i) {
				const auto& entity = logic_entities[i];
				if (!entity.is_active || entity.mesh_index == bud::asset::INVALID_INDEX)
					continue;
				if (entity.mesh_index >= mesh_bounds.size())
					continue;
				const auto wb = mesh_bounds[entity.mesh_index].transform(entity.transform);
				bmin.x = std::min(bmin.x, wb.min.x); bmin.y = std::min(bmin.y, wb.min.y); bmin.z = std::min(bmin.z, wb.min.z);
				bmax.x = std::max(bmax.x, wb.max.x); bmax.y = std::max(bmax.y, wb.max.y); bmax.z = std::max(bmax.z, wb.max.z);
				const float fp = (wb.max.x - wb.min.x) * (wb.max.z - wb.min.z);
				by_footprint.emplace_back(fp, boxes.size());
				boxes.push_back(wb);
				box_entity.push_back(i);
			}

			static std::string s_last_report;
			if (!boxes.empty() && bmax.x > bmin.x) {
				const float scene_fp = std::max((bmax.x - bmin.x) * (bmax.z - bmin.z), 1e-3f);
				const float mid_y = (bmin.y + bmax.y) * 0.5f;

				std::sort(by_footprint.begin(), by_footprint.end(),
					[](const auto& a, const auto& b) { return a.first > b.first; });

				std::vector<size_t> cands;
				std::string signature;
				for (const auto& [fp, slot] : by_footprint) {
					if (fp < 0.25f * scene_fp) break;          // sorted descending: rest are smaller
					const auto& wb = boxes[slot];
					// Only something whose *lowest* point is already above mid-height can
					// lid the scene; a floor or a plinth cannot.
					if (wb.min.y < mid_y) continue;
					cands.push_back(slot);
					signature += logic_entities[box_entity[slot]].asset_path;
					signature += logic_entities[box_entity[slot]].is_cast_shadow ? "1;" : "0;";
				}

				if (!cands.empty() && signature != s_last_report) {
					s_last_report = signature;
					bud::print("[CSM] backdrop candidates (footprint >= 25% of scene {:.1f}m2, entirely above y={:.2f}) - add \"is_cast_shadow\": false to these in the scene file:",
						scene_fp, mid_y);
					for (const size_t slot : cands) {
						const auto& entity = logic_entities[box_entity[slot]];
						const auto& wb = boxes[slot];
						const float fp = (wb.max.x - wb.min.x) * (wb.max.z - wb.min.z);
						bud::print("[CSM]   name={} asset={} footprint={:.1f}m2 ({:.0f}%) thickness={:.2f}m y=[{:.2f},{:.2f}] x=[{:.1f},{:.1f}] z=[{:.1f},{:.1f}] is_cast_shadow={}",
							entity.name, entity.asset_path, fp, 100.0f * fp / scene_fp, wb.max.y - wb.min.y,
							wb.min.y, wb.max.y, wb.min.x, wb.max.x, wb.min.z, wb.max.z, entity.is_cast_shadow);
					}
				}
			}
		}

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
		view_snapshot.fov = scene.main_camera.zoom;	// was never assigned: SceneView::fov read as garbage
		view_snapshot.near_plane = near_plane;
		view_snapshot.far_plane = far_plane;

		view_snapshot.light_dir = bud::math::normalize(scene.directional_light.direction);
		view_snapshot.light_color = scene.directional_light.color;
		view_snapshot.light_intensity = scene.directional_light.intensity;
		view_snapshot.ambient_strength = scene.ambient_strength;

		view_snapshot.show_debug_stats = show_debug_stats;

		view_snapshot.update_matrices();
		view_snapshot.prev_view_proj_matrix = has_last_view_proj ? last_view_proj_matrix : view_snapshot.view_proj_matrix;
		last_view_proj_matrix = view_snapshot.view_proj_matrix;
		has_last_view_proj = true;

		render_inflight_index.store(render_scene_index, std::memory_order_release);

        if (!engine_config.is_puppet_mode) {
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            auto stats = rhi->get_stats();
            const auto seq_state = camera_sequencer.get_state();
            const auto keyframe_count = camera_sequencer.get_keyframe_count();
            const auto playback_index = camera_sequencer.get_playback_index();
            const bool is_paused = camera_sequencer.is_paused();
            const bool is_looping = camera_sequencer.is_looping();
            // Provide occluder fraction setter to the stats UI so user can adjust at runtime
            auto set_occluder = [this](float v) {
                auto cfg = renderer->get_config();
                cfg.heuristic_occluder_fraction = v;
                renderer->set_config(cfg);
            };
            float current_occluder = renderer->get_config().heuristic_occluder_fraction;
            auto set_occluder_enable = [this](bool v) {
                auto cfg = renderer->get_config();
                cfg.heuristic_occluder_enable = v;
                renderer->set_config(cfg);
            };
            bool current_occluder_enable = renderer->get_config().heuristic_occluder_enable;


			auto set_ao_mode = [this](bud::graphics::AOMode mode) {
				auto cfg = renderer->get_config();
				cfg.ao_mode = mode;
				renderer->set_config(cfg);
			};
			bud::graphics::AOMode current_ao_mode = renderer->get_config().ao_mode;

			auto set_ssr_enable = [this](bool v) {
				auto cfg = renderer->get_config();
				cfg.enable_ssr = v;
				renderer->set_config(cfg);
			};
			bool current_ssr_enable = renderer->get_config().enable_ssr;

			auto set_ssgi_enable = [this](bool v) {
				auto cfg = renderer->get_config();
				cfg.enable_ssgi = v;
				renderer->set_config(cfg);
			};
			bool current_ssgi_enable = renderer->get_config().enable_ssgi;

			auto set_ssgi_intensity = [this](float v) {
				auto cfg = renderer->get_config();
				cfg.ssgi_intensity = v;
				renderer->set_config(cfg);
			};
			float current_ssgi_intensity = renderer->get_config().ssgi_intensity;

			auto set_ssgi_blend = [this](float v) {
				auto cfg = renderer->get_config();
				cfg.ssgi_temporal_blend = v;
				renderer->set_config(cfg);
			};
			float current_ssgi_blend = renderer->get_config().ssgi_temporal_blend;

			// Directional-light editing (mutates the runtime scene on the main thread; the
			// light values are copied into view_snapshot at the top of this frame, so HUD
			// changes take effect from the next frame on). Direction is exposed as elevation /
			// azimuth in degrees; angle-driven editing can never produce a zero vector, which
			// keeps the per-frame normalize() and the CSM light-space matrices safe.
			const auto light_dir_len = bud::math::length(scene.directional_light.direction);
			const float current_light_elevation = (light_dir_len > 1e-6f)
				? bud::math::degrees(std::asin(scene.directional_light.direction.y / light_dir_len))
				: 0.0f;
			const float current_light_azimuth = (light_dir_len > 1e-6f)
				? std::min(std::fmod(bud::math::degrees(std::atan2(scene.directional_light.direction.x, scene.directional_light.direction.z)) + 360.0f, 360.0f), 359.0f)
				: 0.0f;

			auto set_light_elevation = [this](float elevation_deg) {
				auto& dir = scene.directional_light.direction;
				const float azimuth_rad = std::atan2(dir.x, dir.z); // keep current azimuth
				const float elevation_rad = bud::math::radians(elevation_deg);
				dir = bud::math::vec3(std::cos(elevation_rad) * std::sin(azimuth_rad),
					std::sin(elevation_rad),
					std::cos(elevation_rad) * std::cos(azimuth_rad));
			};
			auto set_light_azimuth = [this](float azimuth_deg) {
				auto& dir = scene.directional_light.direction;
				const float len = bud::math::length(dir);
				const float elevation_rad = (len > 1e-6f) ? std::asin(dir.y / len) : 0.0f; // keep current elevation
				const float azimuth_rad = bud::math::radians(azimuth_deg);
				dir = bud::math::vec3(std::cos(elevation_rad) * std::sin(azimuth_rad),
					std::sin(elevation_rad),
					std::cos(elevation_rad) * std::cos(azimuth_rad));
			};
			auto set_light_color = [this](bud::math::vec3 color) { scene.directional_light.color = color; };
			auto set_light_intensity = [this](float v) { scene.directional_light.intensity = v; };
			auto set_ambient_strength = [this](float v) { scene.ambient_strength = v; };

			bud::ui::StatsUI::render(stats, view_snapshot.delta_time, seq_state, keyframe_count, playback_index, is_paused, is_looping, show_debug_stats, set_occluder, current_occluder, set_occluder_enable, current_occluder_enable, set_ao_mode, current_ao_mode, set_ssr_enable, current_ssr_enable, set_ssgi_enable, current_ssgi_enable, set_ssgi_intensity, current_ssgi_intensity, set_ssgi_blend, current_ssgi_blend, set_light_elevation, current_light_elevation, set_light_azimuth, current_light_azimuth, set_light_color, scene.directional_light.color, set_light_intensity, scene.directional_light.intensity, set_ambient_strength, scene.ambient_strength);

            ImGui::Render();

            renderer->update_ui_draw_data(ImGui::GetDrawData());
        }

		// 发射渲染任务 (Fire and Forget), Pin to Worker 1 for Vulkan WSI safety
		task_scheduler->spawn_on_thread(1, "RenderTask", [this, render_scene_index, view_snapshot]() mutable {
			renderer->render(render_scenes[render_scene_index], view_snapshot);
			render_inflight_index.store(BudEngine::invalid_render_index, std::memory_order_release);
		}, &render_task_counter);
	}

	bool BudEngine::load_scene_async(const std::string& scene_path, std::function<void()> on_finished) {
		if (!bud::scene::SceneBuilder::load_scene_from_file(scene_path, scene)) {
			bud::eprint("[BudEngine] Failed to load scene file: {}", scene_path);
			if (on_finished)
				on_finished();
			return false;
		}

		load_scene_resources_async(on_finished);
		return true;
	}

	void BudEngine::load_scene_resources_async(std::function<void()> on_finished) {
		std::unordered_set<std::string> unique_asset_paths;
		for (auto& e : scene.entities) {
			if (!e.asset_path.empty()) {
				unique_asset_paths.insert(e.asset_path);
				e.mesh_index = bud::asset::INVALID_INDEX;
			}
		}

		if (unique_asset_paths.empty()) {
			if (on_finished)
				on_finished();
			return;
		}

		auto pending_count = std::make_shared<std::atomic<int>>(static_cast<int>(unique_asset_paths.size()));
		auto finish = std::make_shared<std::function<void()>>(std::move(on_finished));

		if (streaming_manager) {
			// VG assets: streaming manager's callback sets mesh_index and decrements pending_count.
			streaming_manager->set_asset_registered_callback([this, pending_count, finish](const std::string& path, uint32_t mesh_id, const bud::math::AABB& aabb, uint32_t root_group_index, uint32_t base_virtual_page) {
				renderer->register_mesh_bounds(mesh_id, aabb);
				for (auto& ent : scene.entities) {
					if (ent.asset_path == path) {
						ent.mesh_index = mesh_id;
						ent.root_group_index = root_group_index;
						ent.base_virtual_page = base_virtual_page;
					}
				}
				if (pending_count->fetch_sub(1) == 1) {
					bud::print("[BudEngine] Scene resources fully loaded and dispatched ({} entities)", scene.entities.size());
					if (*finish) (*finish)();
				}
			});

			// Non-VG assets: load via traditional mesh path (no Virtual Geometry chunk).
			streaming_manager->set_non_vg_asset_callback([this, pending_count, finish, asset_manager = this->asset_manager.get()](const std::string& path) {
				asset_manager->load_mesh_async(path, [this, pending_count, finish, path](bud::io::MeshData mesh) mutable {
					auto mesh_handle = renderer->upload_mesh(mesh);
					if (mesh_handle.is_valid()) {
						for (auto& ent : scene.entities) {
							if (ent.asset_path == path) {
								ent.mesh_index = mesh_handle.mesh_id;
								ent.material_index = mesh_handle.material_id;
							}
						}
					}
					if (pending_count->fetch_sub(1) == 1) {
						bud::print("[BudEngine] Scene resources fully loaded and dispatched ({} entities)", scene.entities.size());
						if (*finish) (*finish)();
					}
				});
			});

			for (const auto& path : unique_asset_paths) {
				streaming_manager->register_virtual_geometry_async(path);
			}
		}
		else {
			// No streaming manager: fallback to traditional mesh loading for all assets.
			for (const auto& asset_path : unique_asset_paths) {
				asset_manager->load_mesh_async(asset_path, [this, pending_count, finish, asset_path](bud::io::MeshData mesh) mutable {
					auto mesh_handle = renderer->upload_mesh(mesh);
					if (mesh_handle.is_valid()) {
						for (auto& ent : scene.entities) {
							if (ent.asset_path == asset_path) {
								ent.mesh_index = mesh_handle.mesh_id;
								ent.material_index = mesh_handle.material_id;
							}
						}
					}
					if (pending_count->fetch_sub(1) == 1) {
						bud::print("[BudEngine] Scene resources fully loaded and dispatched ({} entities) [fallback]", scene.entities.size());
						if (*finish) (*finish)();
					}
				});
			}
		}
	}
} // namespace bud::engine
