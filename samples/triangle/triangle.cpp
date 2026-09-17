#pragma once

#include <cstring>
#include <exception>
#include <functional>
#include <unordered_map>

#include "triangle.hpp"
#include "src/core/bud.core.hpp"
#include "src/io/bud.io.hpp"
#include "src/runtime/bud.engine.hpp"
#include "src/runtime/bud.scene.io.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"
#include <imgui.h>

using namespace bud::game;


bool TriangleApp::is_fully_loaded() const {
	return true; // Page streaming is async, start rendering immediately
}

void TriangleApp::on_init(const AppConfig& config) {
	bud::print("[TriangleApp] Initialized. Loading scene: {}", config.scene_file);

	auto engine = get_engine();
	auto renderer = engine->get_renderer();

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
		engine->load_scene_async(config.scene_file, [this, engine, renderer]() {
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
				m_robot_avatar = std::move(avatar);
			}
			else {
				bud::eprint("[TriangleApp] Failed to initialize Unitree G1 Avatar controller!");
			}

			bud::print("[TriangleApp] init finished");
		});
	}
	else {
		pending_mesh_loads->store(0);
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
	bool curr_v = input.is_key_down(bud::input::Key::V);
	if (curr_v && !prev_v) {
		if (m_robot_avatar) {
			m_robot_avatar->toggle_camera_mode(cam);
		}
		else {
			const bool first_person = (cam.get_mode() == bud::scene::CameraMode::FirstPerson);
			cam.set_mode(first_person ? bud::scene::CameraMode::FirstPerson
			                          : bud::scene::CameraMode::ThirdPerson);
		}
		bud::print("[Camera] mode: {}", cam.get_mode() == bud::scene::CameraMode::FirstPerson ? "FirstPerson" : "ThirdPerson");
	}
	prev_v = curr_v;

	static bool prev_b = false;
	bool curr_b = input.is_key_down(bud::input::Key::B);
	if (curr_b && !prev_b) {
		if (m_robot_avatar) {
			m_robot_avatar->toggle_render_mode();
		}
	}
	prev_b = curr_b;

	// Process camera rotation input (gamepad and mouse) prior to avatar update
	if (input.is_gamepad_connected()) {
		float rx = input.get_gamepad_axis(bud::input::GamepadAxis::RightX);
		float ry = input.get_gamepad_axis(bud::input::GamepadAxis::RightY);
		if (rx != 0.0f || ry != 0.0f)
			cam.process_mouse_movement(rx * 600.0f * delta_time, ry * 600.0f * delta_time, true);
	}

	float dx, dy;
	input.get_mouse_delta(dx, dy);

	const bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

	if (!imgui_wants_mouse && input.is_mouse_button_down(bud::input::MouseButton::Right)) {
		if (dx != 0.0f || dy != 0.0f)
			cam.process_mouse_movement(dx, dy);
	}

	if (m_robot_avatar) {
		m_robot_avatar->update(delta_time, input, cam);
	}
	else {
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
				float lx = input.get_gamepad_axis(bud::input::GamepadAxis::LeftX);
				float ly = input.get_gamepad_axis(bud::input::GamepadAxis::LeftY);
				move_dir += cam.right * lx;
				move_dir += cam.front * (-ly);
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
	m_robot_avatar.reset();
	bud::print("[TriangleApp] Shutting down.");
}
