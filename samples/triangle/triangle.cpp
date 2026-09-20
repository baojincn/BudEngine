#pragma once

#include <cstring>
#include <exception>
#include <functional>
#include <unordered_map>
#include <thread>

#include "triangle.hpp"
#include "src/core/bud.core.hpp"
#include "src/io/bud.io.hpp"
#include "src/runtime/bud.engine.hpp"
#include "src/runtime/bud.scene.io.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"
#include "src/physics/bud.cloth.hpp"
#include <imgui.h>

using namespace bud::game;


bool TriangleApp::is_fully_loaded() const {
	return true; // Page streaming is async, start rendering immediately
}

void TriangleApp::on_init(const AppConfig& config) {
	bud::print("[TriangleApp] Initialized. Loading scene: {}", config.scene_file);

	auto engine = get_engine();
	auto renderer = engine->get_renderer();

	// Defer the heavy Sponza cloth world build until the simulation is actually running. Building
	// it during scene load competes with physics compilation / mesh uploads and leaves the window
	// with a static UI for tens of seconds. Registration still marks the cloth dirty; the build is
	// kicked off by the first flush after simulation becomes ready.
	if (auto* cloth = renderer->get_cloth_system())
		cloth->set_build_enabled(false);

	// 1. Initial Render Config
	bud::graphics::RenderConfig render_config;
	// Raster-stage bias: applied while rendering INTO the shadow map.
	// Units are vkCmdSetDepthBias factors (device depth units), NOT [0,1] fractions.
	render_config.shadow_bias_constant = 1.5f;
	render_config.shadow_bias_slope = 1.5f;
	render_config.shadow_bias_clamp = 0.0f; // 0 == no clamping (Vulkan convention)
	// Receiver-stage bias: applied when sampling the shadow map (lighting.glsl).
	// Expressed in shadow texels, so it is invariant to cascade size and map resolution.
	render_config.shadow_normal_offset_texels = 1.0f;
	render_config.shadow_receiver_bias_texels = 0.85f;
	render_config.cascade_count = 4;
	render_config.cascade_split_lambda = 0.5;
	render_config.debug_cascades = false;
	render_config.enable_virtual_geometry = true;
	render_config.enable_mesh_shader = true;
	renderer->set_config(render_config);

	// 2. Load Scene via Engine Data-Driven Pipeline
	if (!config.scene_file.empty()) {
		const float ground_plane_height = config.ground_plane_height;
		engine->load_scene_async(config.scene_file, [this, engine, renderer, ground_plane_height]() {
			try {
				auto& scene = engine->get_scene();
				if (auto* sm = engine->get_streaming_manager()) {
					sm->set_unload_radius(scene.streaming_unload_radius);
				}
				auto cur_cfg = renderer->get_config();
				cur_cfg.lod_error_threshold_px = scene.lod_error_threshold_px;
				renderer->set_config(cur_cfg);

				pending_mesh_loads->store(0);

				// Initialize Unitree G1 Robot Avatar
				auto avatar = std::make_unique<bud::robots::RobotAvatarController>();
				bool avatar_ok = avatar->init(engine,
					"Content/Robots/g1_description/g1_29dof.budasset",
					"Content/Robots/g1_description");
				if (avatar_ok) {
					bud::print("[TriangleApp] Unitree G1 Avatar initialized and bound to Sponza scene!");
					avatar->set_camera_view(bud::robots::AvatarCameraView::ThirdPerson, scene.main_camera);

					if (!m_policy_spec_path.empty()) {
						std::string policy_error;
						if (avatar->load_policy(m_policy_path, m_policy_spec_path, policy_error)) {
							bud::print("[TriangleApp] external policy active; WASD/QE or the gamepad drive it");
							if (m_has_policy_command)
								avatar->set_policy_command(m_policy_command);
						}
						else {
							bud::eprint("[TriangleApp] policy load failed: {}", policy_error);
						}
					}

					m_robot_avatar = std::move(avatar);

					if (m_companion_enabled) {
						auto companion = std::make_unique<bud::robots::MicroduckCompanionController>();
						// Spawn Microduck 1.2m behind G1 to clear swing leg envelope
						const bud::math::vec3 duck_spawn(-1.20f, ground_plane_height + 0.16f, 0.0f);
						std::string package_root = std::filesystem::path(m_companion_asset_path).parent_path().string();
						if (companion->init(engine, duck_spawn, m_companion_asset_path, package_root)) {
							bud::print("[TriangleApp] Microduck companion initialized 1.2m behind G1!");
							m_microduck_companion = std::move(companion);
							scene.main_camera.orbit_pitch = -22.0f;
							scene.main_camera.orbit_distance = 2.6f;
							scene.main_camera.target_position = m_robot_avatar->get_pelvis_position() + bud::math::vec3(-0.15f, -0.05f, 0.0f);
							scene.main_camera.update(0.0f);
						}
						else {
							bud::eprint("[TriangleApp] Failed to initialize Microduck companion!");
						}
					}

					// All articulations are registered. Compile the MuJoCo world and warm up policy on background thread
					// so the main render thread never stalls and Windows message pump remains responsive.
					if (m_init_worker_thread.joinable())
						m_init_worker_thread.join();
					m_init_worker_thread = std::thread([this, engine]() {
						if (auto* physics_scene = engine->get_physics_scene()) {
							physics_scene->get_world().prepare_simulation();
						}
						if (m_microduck_companion) {
							if (!m_microduck_companion->start_policy(m_companion_policy_path))
								bud::eprint("[TriangleApp] Microduck policy failed to start (ONNX load error)");
						}
						m_simulation_ready.store(true, std::memory_order_release);
						bud::print("[TriangleApp] Asynchronous simulation preparation & policy warmup finished!");
					});
				}
				else {
					m_simulation_ready.store(true, std::memory_order_release);
					bud::eprint("[TriangleApp] Failed to initialize Unitree G1 Avatar controller!");
				}

				bud::print("[TriangleApp] init finished");
			}
			catch (const std::exception& e) {
				bud::eprint("[TriangleApp] Exception in load_scene_async: {}", e.what());
				std::fprintf(stderr, "[FATAL] Exception in load_scene_async: %s\n", e.what());
				std::fflush(stderr);
			}
			catch (...) {
				bud::eprint("[TriangleApp] Unknown exception in load_scene_async!");
				std::fprintf(stderr, "[FATAL] Unknown exception in load_scene_async!\n");
				std::fflush(stderr);
			}
		});
	}
	else {
		pending_mesh_loads->store(0);
		m_simulation_ready.store(true, std::memory_order_release);
		bud::print("[TriangleApp] init finished");
	}
}

void TriangleApp::on_update(float delta_time) {
	auto engine = get_engine();

	if (engine->is_replay_active())
		return;

	auto& input = bud::input::Input::get();
	auto& scene = engine->get_scene();
	auto& cam = scene.main_camera;


	static bool prev_v = false;
	bool curr_v = input.is_key_down(bud::input::Key::V) ||
	              (input.is_gamepad_connected() && input.is_gamepad_button_down(bud::input::GamepadButton::Y));
	if (curr_v && !prev_v) {
		if (m_robot_avatar)
			m_robot_avatar->toggle_camera_mode(cam);
		else {
			const bool first_person = (cam.get_mode() == bud::scene::CameraMode::FirstPerson);
			cam.set_mode(first_person ? bud::scene::CameraMode::FirstPerson
			                          : bud::scene::CameraMode::ThirdPerson);
		}
		bud::print("[Camera] mode: {}", cam.get_mode() == bud::scene::CameraMode::FirstPerson ? "FirstPerson" : "ThirdPerson");
	}
	prev_v = curr_v;

	static bool prev_b = false;
	bool curr_b = input.is_key_down(bud::input::Key::B) ||
	              (input.is_gamepad_connected() && input.is_gamepad_button_down(bud::input::GamepadButton::B));
	if (curr_b && !prev_b) {
		if (m_robot_avatar)
			m_robot_avatar->toggle_render_mode();
	}
	prev_b = curr_b;

	static bool prev_recenter = false;
	bool curr_recenter = input.is_key_down(bud::input::Key::R) ||
	                     (input.is_gamepad_connected() && (
	                         input.is_gamepad_button_down(bud::input::GamepadButton::RS) ||
	                         input.is_gamepad_button_down(bud::input::GamepadButton::X)));
	if (curr_recenter && !prev_recenter) {
		if (m_robot_avatar)
			m_robot_avatar->recenter_camera(cam);
		else {
			cam.orbit_yaw = 90.0f;
			cam.orbit_pitch = -15.0f;
		}
		bud::print("[Camera] re-centered behind target");
	}
	prev_recenter = curr_recenter;

	static bool prev_c = false;
	bool curr_c = input.is_key_down(bud::input::Key::C);
	if (curr_c && !prev_c && m_microduck_companion) {
		m_camera_focus_companion = !m_camera_focus_companion;
		if (m_camera_focus_companion) {
			cam.orbit_distance = 0.95f;
			cam.orbit_pitch = -12.0f;
			bud::print("[Camera] Focused on Microduck Companion (press C to return focus to G1)");
		}
		else {
			cam.orbit_distance = 2.6f;
			cam.orbit_pitch = -22.0f;
			bud::print("[Camera] Focused on Unitree G1 Avatar");
		}
	}
	prev_c = curr_c;

	// Process camera rotation input (gamepad and mouse) prior to avatar update
	if (input.is_gamepad_connected()) {
		const auto stick_deadzone = [](float val) {
			constexpr float kDeadzone = 0.15f;
			if (std::abs(val) < kDeadzone)
				return 0.0f;
			return (val - std::copysign(kDeadzone, val)) / (1.0f - kDeadzone);
		};
		float rx = stick_deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::RightX));
		float ry = stick_deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::RightY));
		if (rx != 0.0f || ry != 0.0f)
			cam.process_mouse_movement(rx * 600.0f * delta_time, ry * 600.0f * delta_time, true);

		float lt = input.get_gamepad_axis(bud::input::GamepadAxis::LeftTrigger);
		float rt = input.get_gamepad_axis(bud::input::GamepadAxis::RightTrigger);
		if (lt > 0.1f)
			cam.process_mouse_scroll(-lt * 10.0f * delta_time);
		if (rt > 0.1f)
			cam.process_mouse_scroll(rt * 10.0f * delta_time);
	}

	float dx, dy;
	input.get_mouse_delta(dx, dy);

	const bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

	if (!imgui_wants_mouse && input.is_mouse_button_down(bud::input::MouseButton::Right)) {
		if (dx != 0.0f || dy != 0.0f)
			cam.process_mouse_movement(dx, dy);
	}

	float scroll = input.get_mouse_scroll();
	if (!imgui_wants_mouse && scroll != 0.0f)
		cam.process_mouse_scroll(scroll * 0.5f);

	if (!m_simulation_ready.load(std::memory_order_acquire)) {
		if (m_robot_avatar) {
			cam.target_position = m_robot_avatar->get_pelvis_position() + bud::math::vec3(-0.15f, -0.05f, 0.0f);
			cam.update(0.0f);
		}
		return;
	}

	if (!m_simulation_ready_notified) {
		m_simulation_ready_notified = true;
		engine->reset_frame_timer();
		bud::print("[TriangleApp] Simulation active. Frame timer reset, starting simulation steps.");
		// Simulation is live: release the deferred cloth build now (see on_init).
		if (auto* renderer = engine->get_renderer()) {
			if (auto* cloth = renderer->get_cloth_system())
				cloth->set_build_enabled(true);
		}
	}

	if (m_robot_avatar) {
		m_robot_avatar->update(delta_time, input, cam);
		if (m_companion_enabled) {
			if (m_camera_focus_companion && m_microduck_companion) {
				cam.target_position = m_microduck_companion->get_position() + bud::math::vec3(0.0f, 0.15f, 0.0f);
				cam.update(0.0f);
			}
			else {
				cam.target_position = m_robot_avatar->get_pelvis_position() + bud::math::vec3(-0.15f, -0.05f, 0.0f);
				cam.update(0.0f);
			}
		}
	}

	if (m_microduck_companion && m_robot_avatar) {
		const bud::math::vec3 g1_pelvis = m_robot_avatar->get_pelvis_position();
		bud::math::vec3 g1_fwd(1.0f, 0.0f, 0.0f);
		if (auto* g1_robot = m_robot_avatar->get_robot()) {
			const bud::math::quaternion rot = g1_robot->get_link_rotation("pelvis");
			g1_fwd = rot * bud::math::vec3(1.0f, 0.0f, 0.0f);
		}
		m_microduck_companion->update_follower(delta_time, g1_pelvis, g1_fwd);
	}
	else if (!m_robot_avatar) {
		auto* controller = engine->get_character_controller();
		auto* physics = engine->get_physics_scene();
		if (controller && physics) {
			bud::math::vec3 move_dir(0.0f);
			if (input.is_key_down(bud::input::Key::W))
				move_dir += cam.front;
			if (input.is_key_down(bud::input::Key::S))
				move_dir -= cam.front;
			if (input.is_key_down(bud::input::Key::A))
				move_dir -= cam.right;
			if (input.is_key_down(bud::input::Key::D))
				move_dir += cam.right;

			if (input.is_gamepad_connected()) {
				const auto stick_deadzone = [](float val) {
					constexpr float kDeadzone = 0.15f;
					if (std::abs(val) < kDeadzone)
						return 0.0f;
					return (val - std::copysign(kDeadzone, val)) / (1.0f - kDeadzone);
				};
				float lx = stick_deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftX));
				float ly = -stick_deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftY));
				move_dir += cam.right * lx;
				move_dir += cam.front * ly;
			}

			if (glm::length(move_dir) > 0.0f)
				move_dir = glm::normalize(move_dir);

			controller->set_velocity(move_dir * cam.movement_speed);
			controller->update(delta_time);

			if (cam.get_mode() == bud::scene::CameraMode::ThirdPerson)
				cam.target_position = controller->get_position();
			else
				cam.position = controller->get_eye_position();
		}
	}

	if (auto* sm = engine->get_streaming_manager()) {
		sm->update(bud::math::vec3(cam.position.x, cam.position.y, cam.position.z));
	}

	if (m_test_duration > 0.0f && m_robot_avatar && m_robot_avatar->get_robot() && m_robot_avatar->get_robot()->is_simulation()) {
		m_elapsed_time += delta_time;
		const auto pelvis_pos = m_robot_avatar->get_pelvis_position();

		const auto pelvis_rot = m_robot_avatar->get_robot()->get_link_rotation("pelvis");
		float pitch = 0.0f;
		float roll = 0.0f;
		float tilt = 0.0f;
		bud::robots::compute_body_orientation(pelvis_rot, pitch, roll, tilt);

		if (m_elapsed_time - m_last_log_time >= 5.0f || m_elapsed_time >= m_test_duration) {
			m_last_log_time = m_elapsed_time;
			bud::print("[TriangleApp][StandingTest] Time: {:.1f}/{:.1f}s, Pelvis Y: {:.4f}m (range: [{:.4f}, {:.4f}]), Tilt: {:.2f} deg (max: {:.2f} deg)",
			           m_elapsed_time, m_test_duration, pelvis_pos.y, m_min_pelvis_y, m_max_pelvis_y, tilt, m_max_tilt_deg);
		}

		// Steady-state metrics start after the settling window. The drop onto the floor is a transient
		// and must not be folded into the reported height range or maximum tilt.
		if (m_elapsed_time >= 0.5f) {
			m_min_pelvis_y = std::min(m_min_pelvis_y, pelvis_pos.y);
			m_max_pelvis_y = std::max(m_max_pelvis_y, pelvis_pos.y);
			m_max_tilt_deg = std::max(m_max_tilt_deg, tilt);

			if (pelvis_pos.y < 0.60f || tilt > 20.0f) {
				static float last_fail_log = -1.0f;
				if (m_elapsed_time - last_fail_log >= 1.0f) {
					last_fail_log = m_elapsed_time;
					bud::eprint("[TriangleApp][StandingTest] FAILED: Robot lost balance! Pelvis Y: {:.4f}m, Tilt: {:.2f} deg",
					            pelvis_pos.y, tilt);
				}
				m_test_failed = true;
			}
		}

		if (m_elapsed_time >= m_test_duration) {
			bud::print("[TriangleApp][StandingTest] Finished test duration {:.1f}s. Final Result: {}",
			           m_test_duration, m_test_failed ? "FAILED" : "PASSED");
			engine->request_close();
		}
	}
}

void TriangleApp::on_shutdown() {
	if (m_init_worker_thread.joinable())
		m_init_worker_thread.join();
	m_microduck_companion.reset();
	m_robot_avatar.reset();
	bud::print("[TriangleApp] Shutting down.");
}
