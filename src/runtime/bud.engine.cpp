#include <string>
#include <memory>
#include <thread>
#include <chrono>
#include <algorithm>
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
#include "src/physics/bud.cloth.hpp"
#include "src/robots/bud.robot.avatar.hpp"

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
		input_manager->bind_key("ToggleDebug", bud::input::Key::F1);
		input_manager->bind_key("ToggleClusterVis", bud::input::Key::F4);
		input_manager->bind_key("ToggleWireframe", bud::input::Key::F5);
		input_manager->bind_key("ToggleDebugCascades", bud::input::Key::F6);
		input_manager->bind_key("ToggleDebugPhysics", bud::input::Key::F3);
		input_manager->bind_key("ToggleDebugCloth", bud::input::Key::F2);
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

		input_manager->register_action_callback("ToggleDebugPhysics", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto config = renderer->get_config();
				config.debug_physics = !config.debug_physics;
				renderer->set_config(config);
				bud::print("[Physics] Debug physics: {}", config.debug_physics ? "ON" : "OFF");
			}
		});

		input_manager->register_action_callback("ToggleDebugCloth", [this]() {
			if (!camera_sequencer.is_playing()) {
				auto config = renderer->get_config();
				config.debug_cloth = !config.debug_cloth;
				renderer->set_config(config);
				bud::print("[Cloth] Debug visualization: {}", config.debug_cloth ? "ON" : "OFF");
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
						if (physics_scene) {
							physics_scene->step((float)fixed_dt);
							update_physics_debug_overlay();
						}
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

	// Wireframe box outline for every body in the physics SoA, handed to the
	// PhysicsDebugPass. Runs on the logic thread right after the physics step;
	// the cross-thread handoff itself is guarded inside the pass.
	void BudEngine::update_physics_debug_overlay() {
		if (!physics_scene || !renderer)
			return;
		if (!renderer->get_config().debug_physics)
			return;

		std::vector<bud::graphics::PhysicsDebugVertex> dbg_verts;

		if (robot_avatar)
			robot_avatar->get_debug_collision_vertices(dbg_verts);

		const size_t n = physics_scene->size();
		const auto& body_states = physics_scene->get_body_states();
		const auto& pos = body_states.body_positions;
		const auto& he = body_states.body_half_extents;
		const auto& rot = body_states.body_rotations;

		// 12 box edges as a 24 vertex line list.
		static const int box_edges[24] = {
				0,1, 1,2, 2,3, 3,0, 4,5, 5,6, 6,7, 7,4,
				0,4, 1,5, 2,6, 3,7
		};

		for (size_t i = 0; i < n && i < pos.size() && i < he.size(); ++i) {
			auto c = pos[i];
			auto h = he[i];
			if (h.x <= 0.001f || h.y <= 0.001f || h.z <= 0.001f)
				continue;

			bud::math::quaternion q = (i < rot.size()) ? rot[i] : bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);
			bud::math::mat3 r_mat = glm::mat3_cast(q);

			bud::math::vec3 corners[8] = {
				c + r_mat * bud::math::vec3(-h.x,-h.y,-h.z),
				c + r_mat * bud::math::vec3( h.x,-h.y,-h.z),
				c + r_mat * bud::math::vec3( h.x, h.y,-h.z),
				c + r_mat * bud::math::vec3(-h.x, h.y,-h.z),
				c + r_mat * bud::math::vec3(-h.x,-h.y, h.z),
				c + r_mat * bud::math::vec3( h.x,-h.y, h.z),
				c + r_mat * bud::math::vec3( h.x, h.y, h.z),
				c + r_mat * bud::math::vec3(-h.x, h.y, h.z),
			};
			for (int e = 0; e < 24; ++e) {
				auto& p = corners[box_edges[e]];
				dbg_verts.push_back({{p.x, p.y, p.z}, {0.0f, 1.0f, 0.0f}});
			}
		}
		auto push_ring = [&](const bud::math::vec3& center, float ring_r, const bud::math::vec3& color) {
			constexpr int kSeg = 16;
			constexpr float kTwoPi = 6.2831853f;
			for (int i = 0; i < kSeg; ++i) {
				const float a0 = kTwoPi * float(i) / float(kSeg);
				const float a1 = kTwoPi * float(i + 1) / float(kSeg);
				const bud::math::vec3 p0 = center + bud::math::vec3(std::cos(a0) * ring_r, 0.0f, std::sin(a0) * ring_r);
				const bud::math::vec3 p1 = center + bud::math::vec3(std::cos(a1) * ring_r, 0.0f, std::sin(a1) * ring_r);
				dbg_verts.push_back({{p0.x, p0.y, p0.z}, {color.x, color.y, color.z}});
				dbg_verts.push_back({{p1.x, p1.y, p1.z}, {color.x, color.y, color.z}});
			}
		};

		// Character controller capsule wireframe (yellow): suppressed when robot avatar
		// is present because full-body mesh collisions take precedence.
		if (character_controller && !robot_avatar) {
			const float r = character_controller->get_capsule_radius();
			const float hh = character_controller->get_capsule_height() * 0.5f;
			const bud::math::vec3 c = character_controller->get_position();
			const bud::math::vec3 bottom = c - bud::math::vec3(0.0f, hh, 0.0f);
			const bud::math::vec3 top = c + bud::math::vec3(0.0f, hh, 0.0f);
			const bud::math::vec3 color(1.0f, 0.9f, 0.1f);

			// Boundary rings at the two sphere centers (cylinder section ends).
			push_ring(bottom, r, color);
			push_ring(top, r, color);

			// Hemispherical cap rings at 25/50/70 degrees latitude: the shrinking
			// radii convey the rounded end caps.
			for (float deg : {25.0f, 50.0f, 70.0f}) {
				const float a = bud::math::radians(deg);
				const float ring_r = r * std::cos(a);
				const float dy = r * std::sin(a);
				push_ring(bottom - bud::math::vec3(0.0f, dy, 0.0f), ring_r, color);
				push_ring(top + bud::math::vec3(0.0f, dy, 0.0f), ring_r, color);
			}

			// Vertical silhouette lines (every 45 degrees) spanning the cylinder.
			for (int i = 0; i < 8; ++i) {
				const float a = 6.2831853f * float(i) / 8.0f;
				const bud::math::vec3 off(std::cos(a) * r, 0.0f, std::sin(a) * r);
				const bud::math::vec3 p0 = bottom + off;
				const bud::math::vec3 p1 = top + off;
				dbg_verts.push_back({{p0.x, p0.y, p0.z}, {color.x, color.y, color.z}});
				dbg_verts.push_back({{p1.x, p1.y, p1.z}, {color.x, color.y, color.z}});
			}
		}

		renderer->update_physics_debug_vertices(dbg_verts);
	}


	void BudEngine::step(float fixed_dt, GameLogic perform_game_logic) {
		task_scheduler->pump_main_thread_tasks();
		handle_events();

		// Update logic once
		if (perform_game_logic) {
			bud::threading::Counter logic_counter;
			task_scheduler->spawn("GameLogic_Step", [&]() {
				if (physics_scene) {
					physics_scene->step((float)fixed_dt);
					update_physics_debug_overlay();
				}
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

					bool is_cloth = bud::physics::is_cloth_name_or_path(entity.name) || bud::physics::is_cloth_name_or_path(entity.asset_path);

					bud::math::mat4 prev_world_matrix = world_matrix;
					if (entity.has_prev_transform)
						prev_world_matrix = entity.prev_transform;

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
						entity.is_receive_shadow,
						is_cloth,
						prev_world_matrix
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

		view_snapshot.unjittered_proj_matrix = view_snapshot.proj_matrix;

		if (render_config.enable_taa && renderer && renderer->is_taa_ready()) {
			auto halton_sequence = [](uint32_t index, uint32_t base) -> float {
				float f = 1.0f;
				float r = 0.0f;
				while (index > 0) {
					f /= static_cast<float>(base);
					r += f * static_cast<float>(index % base);
					index /= base;
				}
				return r;
			};

			const uint32_t sample_idx = (taa_frame_index % 16) + 1;
			++taa_frame_index;

			const float jitter_x = (halton_sequence(sample_idx, 2) - 0.5f) * render_config.taa_jitter_scale;
			const float jitter_y = (halton_sequence(sample_idx, 3) - 0.5f) * render_config.taa_jitter_scale;

			view_snapshot.jitter_offset = bud::math::vec2(jitter_x, jitter_y);

			const float jitter_ndc_x = (2.0f * jitter_x) / view_snapshot.viewport_width;
			const float jitter_ndc_y = (2.0f * jitter_y) / view_snapshot.viewport_height;

			view_snapshot.jitter_ndc = bud::math::vec2(jitter_ndc_x, jitter_ndc_y);

			view_snapshot.proj_matrix[2][0] -= jitter_ndc_x;
			view_snapshot.proj_matrix[2][1] -= jitter_ndc_y;
		}
		else {
			view_snapshot.jitter_offset = bud::math::vec2(0.0f);
			view_snapshot.jitter_ndc = bud::math::vec2(0.0f);
		}

		view_snapshot.update_matrices();
		view_snapshot.prev_view_proj_matrix = has_last_view_proj ? last_view_proj_matrix : view_snapshot.unjittered_view_proj_matrix;
		view_snapshot.prev_unjittered_view_proj_matrix = view_snapshot.prev_view_proj_matrix;
		last_view_proj_matrix = view_snapshot.unjittered_view_proj_matrix;
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

			auto set_taa_enable = [this](bool v) {
				auto cfg = renderer->get_config();
				cfg.enable_taa = v;
				renderer->set_config(cfg);
			};
			bool current_taa_enable = renderer->get_config().enable_taa;

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
			constexpr float epsilon_threshold = 1e-6f;
			constexpr float degrees_full_turn = 360.0f;
			constexpr float minimum_elevation_degrees = 0.0f;
			constexpr float maximum_elevation_degrees = 90.0f;
			constexpr float minimum_azimuth_degrees = 0.0f;
			constexpr float maximum_azimuth_degrees = 359.0f;
			constexpr float minimum_normalized_clamping_value = -1.0f;
			constexpr float maximum_normalized_clamping_value = 1.0f;

			static float saved_light_elevation = 63.0f;
			static float saved_light_azimuth = 51.0f;
			static bool saved_angles_initialized = false;

			const auto& light_dir = scene.directional_light.direction;
			const float light_direction_length = bud::math::length(light_dir);
			if (light_direction_length > epsilon_threshold)
			{
				// scene.directional_light.direction points toward the light/sun (L in shader).
				// Elevation angle: angle above the horizontal XZ plane [0.0, 90.0]
				const float clamped_sun_height = std::clamp(
					light_dir.y / light_direction_length,
					minimum_normalized_clamping_value,
					maximum_normalized_clamping_value
				);
				float elev_deg = bud::math::degrees(std::asin(clamped_sun_height));
				if (elev_deg < minimum_elevation_degrees)
				{
					elev_deg = minimum_elevation_degrees;
				}

				if (elev_deg > maximum_elevation_degrees)
				{
					elev_deg = maximum_elevation_degrees;
				}

				// Only re-extract azimuth if horizontal length is non-zero (avoids pole singularity at 90 deg)
				const float horizontal_length = std::sqrt(light_dir.x * light_dir.x + light_dir.z * light_dir.z);
				if (horizontal_length > epsilon_threshold)
				{
					float azimuth_degrees = bud::math::degrees(std::atan2(light_dir.x, light_dir.z));
					azimuth_degrees = std::fmod(azimuth_degrees, degrees_full_turn);
					if (azimuth_degrees < 0.0f)
					{
						azimuth_degrees += degrees_full_turn;
					}

					if (azimuth_degrees >= degrees_full_turn)
					{
						azimuth_degrees = 0.0f;
					}

					if (azimuth_degrees > maximum_azimuth_degrees)
					{
						azimuth_degrees = maximum_azimuth_degrees;
					}

					saved_light_azimuth = azimuth_degrees;
				}

				saved_light_elevation = elev_deg;
				saved_angles_initialized = true;
			}

			float current_light_elevation = saved_light_elevation;
			float current_light_azimuth = saved_light_azimuth;

			auto set_light_elevation = [this, minimum_elevation_degrees, maximum_elevation_degrees](float elevation_deg) {
				saved_light_elevation = std::clamp(elevation_deg, minimum_elevation_degrees, maximum_elevation_degrees);
				auto& dir = scene.directional_light.direction;
				const float azimuth_rad = bud::math::radians(saved_light_azimuth);
				const float elevation_rad = bud::math::radians(saved_light_elevation);
				dir = bud::math::vec3(std::cos(elevation_rad) * std::sin(azimuth_rad),
					std::sin(elevation_rad),
					std::cos(elevation_rad) * std::cos(azimuth_rad));
			};
			auto set_light_azimuth = [this, minimum_azimuth_degrees, maximum_azimuth_degrees](float azimuth_deg) {
				saved_light_azimuth = std::clamp(azimuth_deg, minimum_azimuth_degrees, maximum_azimuth_degrees);
				auto& dir = scene.directional_light.direction;
				const float elevation_rad = bud::math::radians(saved_light_elevation);
				const float azimuth_rad = bud::math::radians(saved_light_azimuth);
				dir = bud::math::vec3(std::cos(elevation_rad) * std::sin(azimuth_rad),
					std::sin(elevation_rad),
					std::cos(elevation_rad) * std::cos(azimuth_rad));
			};
			auto set_light_color = [this](bud::math::vec3 color) { scene.directional_light.color = color; };
			auto set_light_intensity = [this](float v) { scene.directional_light.intensity = v; };
			auto set_ambient_strength = [this](float v) { scene.ambient_strength = v; };

			auto set_cloth_config = [this](const bud::physics::ClothConfig& cfg) {
				auto render_cfg = renderer->get_config();
				render_cfg.cloth_config = cfg;
				renderer->set_config(render_cfg);
				if (renderer && renderer->get_cloth_system())
					renderer->get_cloth_system()->set_config(cfg);
			};
			auto current_cloth_config = renderer->get_config().cloth_config;

			bud::ui::StatsUI::render(stats, view_snapshot.delta_time, seq_state, keyframe_count, playback_index, is_paused, is_looping, show_debug_stats, set_occluder, current_occluder, set_occluder_enable, current_occluder_enable, set_ao_mode, current_ao_mode, set_ssr_enable, current_ssr_enable, set_taa_enable, current_taa_enable, set_ssgi_enable, current_ssgi_enable, set_ssgi_intensity, current_ssgi_intensity, set_ssgi_blend, current_ssgi_blend, set_light_elevation, current_light_elevation, set_light_azimuth, current_light_azimuth, set_light_color, scene.directional_light.color, set_light_intensity, scene.directional_light.intensity, set_ambient_strength, scene.ambient_strength, set_cloth_config, current_cloth_config);

			ImGui::Render();

            renderer->update_ui_draw_data(ImGui::GetDrawData());
        }

		// Update the bound character capsule collider for cloth interaction.
		if (renderer && renderer->get_cloth_system()) {
			// Rebuild the cloth SimWorld if assets / colliders changed this frame (main thread).
			renderer->get_cloth_system()->flush_pending();

			const bool spectator_mode = scene.main_camera.get_mode() == bud::scene::CameraMode::FreeFly;

			if (spectator_mode || !character_controller) {
				renderer->get_cloth_system()->set_capsule_collider({}, false);
			}
			else {
				bud::physics::CapsuleCollider capsule{};
				float radius = character_controller->get_capsule_radius();
				float half_height = character_controller->get_capsule_height() * 0.5f;
				bud::math::vec3 pos = character_controller->get_position();
				bud::math::vec3 vel = character_controller->get_linear_velocity();

				capsule.p_bottom = pos - bud::math::vec3(0.0f, half_height, 0.0f);
				capsule.radius = radius;
				// Cloth-only extension: reach ~2.05 m above the feet so hanging curtain
				// hems (raised porches put them near 2 m) are always inside the cloth
				// collision volume. The Jolt MOVEMENT capsule stays untouched.
				capsule.p_top = pos + bud::math::vec3(0.0f, half_height + 0.45f, 0.0f);
				capsule.friction = 0.25f;
				capsule.velocity = vel;

				renderer->get_cloth_system()->set_capsule_collider(capsule, true);

			}
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

		// Initialize physics scene.
		// Tear the previous scene down FIRST: ~PhysicsScene unregisters the Jolt
		// types and deletes the process-wide JPH::Factory, so an old scene that is
		// destroyed after a new one has been initialised would leave the new scene
		// running without a factory (scene reload = broken/empty physics).
		character_controller.reset();
		physics_scene.reset();
		physics_scene = std::make_unique<bud::physics::PhysicsScene>();
		bud::physics::PhysicsWorldConfig phys_config{};
		phys_config.backend = engine_config.physics_backend;
		phys_config.enable_ground_plane = engine_config.enable_ground_plane;
		phys_config.ground_plane_height = engine_config.ground_plane_height;
		phys_config.asset_root = "Content";
		physics_scene->init(phys_config);

		bud::print("[BudEngine] Physics scene initialized ({}), ready for bodies.",
		           engine_config.physics_backend == bud::physics::PhysicsBackend::Mujoco ? "MuJoCo" : "Jolt");

		// Create character controller
		character_controller = std::make_unique<bud::scene::CharacterController>();
		character_controller->init(physics_scene.get(), scene.main_camera.position);

		// Wrap on_finished to create physics bodies after mesh bounds are loaded
		auto wrapped_finish = [this, cb = std::move(on_finished)]() {
			auto bounds = renderer->get_mesh_bounds_snapshot();
			int skipped = 0, added = 0, dropped = 0, data_driven = 0;

			for (auto& entity : scene.entities) {
				if (!entity.is_active || !entity.enable_physics || !entity.is_static) {
					skipped++;
					continue;
				}
				if (entity.collider.type == bud::scene::ColliderType::None) {
					skipped++;
					continue;
				}
				if (bud::physics::is_cloth_name_or_path(entity.name) || bud::physics::is_cloth_name_or_path(entity.asset_path)) {
					skipped++;
					continue;
				}
				// If this asset already has a baked CollisionLOD chunk loaded, it's already registered
				if (collision_loaded_assets.find(entity.asset_path) != collision_loaded_assets.end()) {
					data_driven++;
					continue;
				}

				// Only explicit authored box colliders should generate box rigid bodies
				if (entity.collider.type == bud::scene::ColliderType::Box) {
					bud::math::vec3 half = entity.collider.box_half_extent;
					if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f) {
						if (entity.mesh_index < bounds.size())
							half = bounds[entity.mesh_index].transform(entity.transform).size() * 0.5f;
						else
							half = bud::math::vec3(0.5f);
					}
					bud::physics::RigidBodyDesc desc;
					desc.motion_type = entity.is_static ? bud::physics::MotionType::Static : bud::physics::MotionType::Dynamic;
					desc.shape.type = bud::physics::ShapeType::Box;
					desc.shape.half_extent = half;
					desc.position = bud::math::vec3(entity.transform[3]);
					desc.material.friction = entity.collider.friction;
					desc.material.restitution = entity.collider.restitution;
					if (physics_scene->add_rigid_body(desc).is_valid())
						added++;
					else
						dropped++;
				} else {
					skipped++;
				}
			}

			bud::print("[Physics] Scene colliders ready: data_driven={}, explicit_boxes={}, dropped={}, skipped={}, total_bodies={}",
			           data_driven, added, dropped, skipped, physics_scene->size());

			// Sync all colliders to Jolt so the broadphase and narrowphase queries (like raycasts) are valid
			physics_scene->sync_to_jolt();

			// Feed static colliders to cloth system for cloth-rigid interaction
			if (renderer && renderer->get_cloth_system()) {
				std::vector<bud::physics::BoxCollider> cloth_boxes;
				size_t num_bodies = physics_scene->size();
				const auto& body_states = physics_scene->get_body_states();
				cloth_boxes.reserve(num_bodies);
				for (size_t i = 0; i < num_bodies; ++i) {
					if (body_states.body_flags[i] & bud::physics::BODY_FLAG_STATIC)
						cloth_boxes.push_back({ body_states.body_positions[i], body_states.body_half_extents[i] });
				}
				renderer->get_cloth_system()->set_scene_colliders(std::move(cloth_boxes));
			}

			// Start in a walkable mode: the scene JSON may author FreeFly, but the
			// game loop is built around FP/TP (character-driven cloth interaction).
			scene.main_camera.set_mode(bud::scene::CameraMode::FirstPerson);

			// Snap character capsule feet to the static ground surface via downward raycast.
			if (character_controller && physics_scene->size() > 0) {
				character_controller->snap_to_ground();
				bud::print("[Physics] character snapped to ground: ({:.2f}, {:.2f}, {:.2f})",
				           character_controller->get_position().x,
				           character_controller->get_position().y,
				           character_controller->get_position().z);
			}

			{
				std::scoped_lock lock(collision_mutex);
				if (renderer)
					renderer->set_static_physics_debug_vertices(static_collision_debug_vertices);
			}

			if (cb)
				cb();
		};

		load_scene_resources_async(wrapped_finish);
		return true;
	}

	void BudEngine::register_collision_asset(const std::string& path, const std::vector<char>& data) {
		if (data.size() < sizeof(bud::asset::CollisionChunkHeader))
			return;

		bud::asset::CollisionChunkHeader header{};
		std::memcpy(&header, data.data(), sizeof(header));
		if (header.magic != bud::asset::COLLISION_CHUNK_MAGIC)
			return;
		if (header.version != bud::asset::COLLISION_CHUNK_VERSION) {
			bud::eprint("[Physics] Unsupported collision chunk version {} for '{}'", header.version, path);
			return;
		}
		if (header.index_count == 0 || header.index_count % 3 != 0 || header.vertex_count == 0) {
			bud::eprint("[Physics] Invalid collision counts for '{}': vertices={}, indices={}",
						path, header.vertex_count, header.index_count);
			return;
		}

		const size_t vert_bytes = static_cast<size_t>(header.vertex_count) * sizeof(bud::math::vec3);
		const size_t idx_bytes = static_cast<size_t>(header.index_count) * sizeof(uint32_t);
		if (vert_bytes > data.size() - sizeof(header) ||
			idx_bytes > data.size() - sizeof(header) - vert_bytes) {
			bud::eprint("[Physics] Truncated collision chunk for '{}'", path);
			return;
		}

		std::vector<bud::math::vec3> local_vertices(header.vertex_count);
		const char* src = data.data() + sizeof(header);
		if (vert_bytes > 0)
			std::memcpy(local_vertices.data(), src, vert_bytes);
		src += vert_bytes;

		std::vector<uint32_t> local_indices(header.index_count);
		if (idx_bytes > 0)
			std::memcpy(local_indices.data(), src, idx_bytes);

		for (const auto& vertex : local_vertices) {
			if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
				bud::eprint("[Physics] Non-finite vertex in collision chunk '{}'", path);
				return;
			}
		}
		for (uint32_t index : local_indices) {
			if (index >= local_vertices.size()) {
				bud::eprint("[Physics] Out-of-range index {} in collision chunk '{}'", index, path);
				return;
			}
		}

		bud::asset::CollisionShapeType baked_shape = static_cast<bud::asset::CollisionShapeType>(header.shape_type);
		{
			std::scoped_lock lock(collision_mutex);
			collision_loaded_assets.insert(path);

			for (auto& entity : scene.entities) {
				if (entity.asset_path != path)
					continue;
				if (!entity.is_active || !entity.enable_physics)
					continue;
				if (entity.collider.type == bud::scene::ColliderType::None)
					continue;
				if (bud::physics::is_cloth_name_or_path(entity.name) || bud::physics::is_cloth_name_or_path(entity.asset_path))
					continue;

				bud::physics::RigidBodyDesc desc;
				desc.motion_type = entity.is_static ? bud::physics::MotionType::Static : bud::physics::MotionType::Dynamic;
				desc.material.friction = entity.collider.friction;
				desc.material.restitution = entity.collider.restitution;

				std::vector<bud::math::vec3> world_vertices;
				world_vertices.reserve(local_vertices.size());
				bool non_finite_transform = false;
				for (const auto& lv : local_vertices) {
					bud::math::vec4 wv = entity.transform * bud::math::vec4(lv, 1.0f);
					if (!std::isfinite(wv.x) || !std::isfinite(wv.y) || !std::isfinite(wv.z)) {
						non_finite_transform = true;
						break;
					}
					world_vertices.push_back(bud::math::vec3(wv.x, wv.y, wv.z));
				}
				if (non_finite_transform) {
					bud::eprint("[Physics] Non-finite world vertex from transform of '{}' - collision skipped", entity.name);
					continue;
				}

				// Sliver/degenerate triangles MUST NOT reach Jolt's MeshShape: its EPA
				// expansion overruns a fixed-size pool on them (Jolt documents this in
				// EPAPenetrationDepth.h) and the failure only shows up in release builds,
				// where the guarding assertions are compiled out. Meshopt simplification
				// produces slivers routinely, so keep triangles that are:
				//   - not collapsed (distinct indices),
				//   - flat enough in both absolute area and minimum height
				//     (min height = 2 * area / longest edge),
				//   - not extreme slivers (longest edge / min height bounded).
				std::vector<uint32_t> valid_indices;
				valid_indices.reserve(local_indices.size());
				uint32_t dropped_degenerate = 0;
				for (size_t i = 0; i + 2 < local_indices.size(); i += 3) {
					const uint32_t i0 = local_indices[i];
					const uint32_t i1 = local_indices[i + 1];
					const uint32_t i2 = local_indices[i + 2];
					if (i0 == i1 || i1 == i2 || i0 == i2) {
						++dropped_degenerate;
						continue;
					}
					const auto& a = world_vertices[i0];
					const auto& b = world_vertices[i1];
					const auto& c = world_vertices[i2];
					const auto ab = b - a;
					const auto ac = c - a;
					const auto bc = c - b;
					const float area2 = glm::length(glm::cross(ab, ac)); // 2 * area
					const float longest = std::sqrt(std::max(glm::dot(ab, ab),
						std::max(glm::dot(ac, ac), glm::dot(bc, bc))));
					const float min_height = (longest > 1e-8f) ? (area2 / longest) : 0.0f;
					constexpr float kMinArea2 = 1.0e-6f;   // 2 * 0.5 mm^2
					constexpr float kMinHeight = 5.0e-3f;  // 5 mm
					constexpr float kMaxAspect = 40.0f;    // longest edge / min height
					if (area2 < kMinArea2 || min_height < kMinHeight ||
						(min_height > 0.0f && longest / min_height > kMaxAspect)) {
						++dropped_degenerate;
						continue;
					}
					valid_indices.insert(valid_indices.end(), { i0, i1, i2 });
				}
				if (dropped_degenerate > 0) {
					bud::print("[Physics] '{}' dropped {} degenerate/sliver collision triangles",
							   entity.name, dropped_degenerate);
				}

				if (valid_indices.empty()) {
					bud::eprint("[Physics] Skipping collision asset '{}' for entity '{}': no non-degenerate triangles",
								path, entity.name);
					continue;
				}

				// Static colliders keep using the baked CollisionLOD triangle meshes.
				bool use_mesh = (entity.is_static && (entity.collider.type == bud::scene::ColliderType::Mesh ||
								(entity.collider.type == bud::scene::ColliderType::Auto && baked_shape == bud::asset::CollisionShapeType::Mesh)));

				if (use_mesh) {
					desc.shape.type = bud::physics::ShapeType::Mesh;
				} else {
					desc.shape.type = bud::physics::ShapeType::ConvexHull;
				}

				if (entity.is_static) {
					auto make_edge_key = [](uint32_t a, uint32_t b) -> uint64_t {
						if (a > b)
							std::swap(a, b);
						return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
					};
					std::unordered_set<uint64_t> unique_edges;
					unique_edges.reserve(local_indices.size());
					for (size_t i = 0; i + 2 < local_indices.size(); i += 3) {
						unique_edges.insert(make_edge_key(local_indices[i], local_indices[i + 1]));
						unique_edges.insert(make_edge_key(local_indices[i + 1], local_indices[i + 2]));
						unique_edges.insert(make_edge_key(local_indices[i + 2], local_indices[i]));
					}
					constexpr float col_r = 0.2f;
					constexpr float col_g = 0.85f;
					constexpr float col_b = 0.3f;
					for (uint64_t edge : unique_edges) {
						uint32_t idx_a = static_cast<uint32_t>(edge >> 32);
						uint32_t idx_b = static_cast<uint32_t>(edge & 0xFFFFFFFFull);
						if (idx_a < world_vertices.size() && idx_b < world_vertices.size()) {
							const auto& va = world_vertices[idx_a];
							const auto& vb = world_vertices[idx_b];
							static_collision_debug_vertices.push_back({{va.x, va.y, va.z}, {col_r, col_g, col_b}});
							static_collision_debug_vertices.push_back({{vb.x, vb.y, vb.z}, {col_r, col_g, col_b}});
						}
					}
				}

				desc.shape.vertices = std::move(world_vertices);
				desc.shape.indices = std::move(valid_indices);
				desc.position = bud::math::vec3(0.0f);
				desc.rotation = bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);

				auto handle = physics_scene->add_rigid_body(desc);
				if (handle.is_valid()) {
					bud::print("[Physics] Instantiated Data-Driven CollisionLOD for '{}' (type={}, verts={}, tris={})",
					           entity.name, use_mesh ? "Mesh" : "ConvexHull",
					           desc.shape.vertices.size(), desc.shape.indices.size() / 3);
				}
			}
		}
	}

	void BudEngine::load_scene_resources_async(std::function<void()> on_finished) {
		{
			std::scoped_lock lock(collision_mutex);
			collision_loaded_assets.clear();
			static_collision_debug_vertices.clear();
			if (renderer)
				renderer->set_static_physics_debug_vertices({});
		}

		// Shared cloth registration: expand culling bounds (traditional draw path must
		// stay non-page-based) and load the ClothPhysics chunk. The mega-buffer vertex
		// offset comes straight from the upload handle (reserved at enqueue time).
		auto setup_cloth_asset = [this](const std::string& path, const bud::graphics::MeshAssetHandle& handle, const bud::io::MeshData& mesh) {
			bud::math::AABB cloth_bounds{};
			for (const auto& v : mesh.vertices) {
				cloth_bounds.merge(bud::math::vec3(v.pos[0], v.pos[1], v.pos[2]));
			}
			cloth_bounds.min -= bud::math::vec3(1.5f);
			cloth_bounds.max += bud::math::vec3(1.5f);
			renderer->update_mesh_bounds(handle.mesh_id, cloth_bounds);

			const uint32_t mesh_id = handle.mesh_id;
			const int32_t vertex_offset = handle.vertex_offset;

			this->asset_manager->load_budasset_async(path, [this, path, mesh_id, vertex_offset](std::shared_ptr<bud::io::BudAssetPackage> pkg) {
				if (!pkg)
					return;
				if (pkg->find_chunk(bud::asset::AssetChunkType::ClothPhysics)) {
					this->asset_manager->load_budasset_chunk_async(pkg, bud::asset::AssetChunkType::ClothPhysics, [this, path, mesh_id, vertex_offset](std::vector<char> data) {
						if (data.empty())
							return;
						if (renderer->get_cloth_system()) {
							renderer->get_cloth_system()->register_cloth_asset(path, mesh_id, vertex_offset, data);
						}
					});
				}
			});
		};

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

		auto pending_count = std::make_shared<std::atomic<int>>(static_cast<int>(unique_asset_paths.size() * 2));
		auto finish = std::make_shared<std::function<void()>>(std::move(on_finished));

		// Asynchronously load CollisionLOD chunks for all unique assets
		for (const auto& path : unique_asset_paths) {
			this->asset_manager->load_budasset_async(path, [this, path, pending_count, finish](std::shared_ptr<bud::io::BudAssetPackage> pkg) {
				if (!pkg) {
					if (pending_count->fetch_sub(1) == 1) {
						bud::print("[BudEngine] Scene resources and colliders fully loaded and dispatched ({} entities)", scene.entities.size());
						if (*finish)
							(*finish)();
					}
					return;
				}
				if (pkg->find_chunk(bud::asset::AssetChunkType::Collision)) {
					this->asset_manager->load_budasset_chunk_async(pkg, bud::asset::AssetChunkType::Collision, [this, path, pending_count, finish](std::vector<char> data) {
						if (!data.empty())
							this->register_collision_asset(path, data);
						if (pending_count->fetch_sub(1) == 1) {
							bud::print("[BudEngine] Scene resources and colliders fully loaded and dispatched ({} entities)", scene.entities.size());
							if (*finish)
								(*finish)();
						}
					});
				} else {
					if (pending_count->fetch_sub(1) == 1) {
						bud::print("[BudEngine] Scene resources and colliders fully loaded and dispatched ({} entities)", scene.entities.size());
						if (*finish)
							(*finish)();
					}
				}
			});
		}

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
			streaming_manager->set_non_vg_asset_callback([this, pending_count, finish, asset_manager = this->asset_manager.get(), setup_cloth_asset](const std::string& path) {
				asset_manager->load_mesh_async(path, [this, pending_count, finish, path, setup_cloth_asset](bud::io::MeshData mesh) mutable {
					auto mesh_handle = renderer->upload_mesh(mesh);
					if (mesh_handle.is_valid()) {
						for (auto& ent : scene.entities) {
							if (ent.asset_path == path) {
								ent.mesh_index = mesh_handle.mesh_id;
								ent.material_index = mesh_handle.material_id;
								if (bud::physics::is_cloth_name_or_path(path) || bud::physics::is_cloth_name_or_path(ent.name)) {
									// Cloth renders through the traditional Range-B path with
									// world-space skinned vertices; never VG / page-based.
									ent.is_static = false;
									ent.root_group_index = bud::asset::INVALID_INDEX;
									ent.base_virtual_page = bud::asset::INVALID_INDEX;
								}
							}
						}

						if (bud::physics::is_cloth_name_or_path(path)) {
							setup_cloth_asset(path, mesh_handle, mesh);
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
				asset_manager->load_mesh_async(asset_path, [this, pending_count, finish, asset_path, setup_cloth_asset](bud::io::MeshData mesh) mutable {
					auto mesh_handle = renderer->upload_mesh(mesh);
					if (mesh_handle.is_valid()) {
						for (auto& ent : scene.entities) {
							if (ent.asset_path == asset_path) {
								ent.mesh_index = mesh_handle.mesh_id;
								ent.material_index = mesh_handle.material_id;
								if (bud::physics::is_cloth_name_or_path(asset_path) || bud::physics::is_cloth_name_or_path(ent.name)) {
									// Cloth renders through the traditional Range-B path with
									// world-space skinned vertices; never VG / page-based.
									ent.is_static = false;
									ent.root_group_index = bud::asset::INVALID_INDEX;
									ent.base_virtual_page = bud::asset::INVALID_INDEX;
								}
							}
						}

						if (bud::physics::is_cloth_name_or_path(asset_path)) {
							setup_cloth_asset(asset_path, mesh_handle, mesh);
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
