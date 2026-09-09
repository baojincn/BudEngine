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
	render_config.shadow_bias_constant = 2.0f;
	render_config.shadow_bias_slope = 1.75f;
	render_config.shadow_bias_clamp = 0.0f; // 0 == no clamping (Vulkan convention)
	// Receiver-stage bias: applied when sampling the shadow map (lighting.glsl).
	// Expressed in shadow texels, so it is invariant to cascade size and map resolution.
	render_config.shadow_normal_offset_texels = 1.0f;
	render_config.shadow_receiver_bias_texels = 1.5f;
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

	if (engine->is_replay_active()) return;

	auto& input = bud::input::Input::get();
	auto& scene = engine->get_scene();
	auto& cam = scene.main_camera;
	auto* controller = engine->get_character_controller();
	auto* physics = engine->get_physics_scene();

	static bool prev_v = false;
	bool curr_v = input.is_key_down(bud::input::Key::V);
	if (curr_v && !prev_v) {
		const bool first_person = (cam.get_mode() == bud::scene::CameraMode::FirstPerson);
		cam.set_mode(first_person ? bud::scene::CameraMode::FirstPerson
		                          : bud::scene::CameraMode::ThirdPerson);
		bud::print("[Camera] mode: {}", first_person ? "FirstPerson" : "ThirdPerson");
	}
	prev_v = curr_v;

	if (auto* sm = engine->get_streaming_manager()) {
		sm->update(bud::math::vec3(cam.position.x, cam.position.y, cam.position.z));
	}

	// Character movement via CharacterController
	if (controller && physics) {
		bud::math::vec3 move_dir(0.0f);
		if (input.is_key_down(bud::input::Key::W)) move_dir += cam.front;
		if (input.is_key_down(bud::input::Key::S)) move_dir -= cam.front;
		if (input.is_key_down(bud::input::Key::A)) move_dir -= cam.right;
		if (input.is_key_down(bud::input::Key::D)) move_dir += cam.right;

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

		// Sync camera from controller
		if (cam.get_mode() == bud::scene::CameraMode::ThirdPerson) {
			cam.target_position = controller->get_position();
		} else {
			cam.position = controller->get_eye_position();
		}

		// Gamepad right stick for camera rotation
		if (input.is_gamepad_connected()) {
			float rx = input.get_gamepad_axis(bud::input::GamepadAxis::RightX);
			float ry = input.get_gamepad_axis(bud::input::GamepadAxis::RightY);
			if (rx != 0.0f || ry != 0.0f)
				cam.process_mouse_movement(rx * 600.0f * delta_time, ry * 600.0f * delta_time, true);
		}
	}

	float dx, dy;
	input.get_mouse_delta(dx, dy);

	const bool imgui_wants_mouse = ImGui::GetIO().WantCaptureMouse;

	if (!imgui_wants_mouse && input.is_mouse_button_down(bud::input::MouseButton::Right)) {
		if (dx != 0.0f || dy != 0.0f)
			cam.process_mouse_movement(dx, dy);
	}
}

void TriangleApp::on_shutdown() {
	bud::print("[TriangleApp] Shutting down.");
}

pybind11::array_t<uint8_t> TriangleApp::step(float dt) {
	step_puppet(dt);

	auto engine = get_engine();
	const void* pixels = engine->get_readback_pixels();
	if (!pixels)
		return pybind11::array_t<uint8_t>();

	auto& engine_config = engine->get_engine_config();
	pybind11::array_t<uint8_t> result({ engine_config.height, engine_config.width, 4 });
    auto req = result.request();
    std::memcpy(req.ptr, pixels, (uint64_t)engine_config.width * engine_config.height * 4);

	return result;
}
