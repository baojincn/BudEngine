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
				sm->set_unload_radius(scene.streaming_unload_radius * bud::core::units::m);
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

	static bool prev_v = false;
	bool curr_v = input.is_key_down(bud::input::Key::V);
	if (curr_v && !prev_v) {
		if (cam.get_mode() == bud::scene::CameraMode::FreeFly)
			cam.set_mode(bud::scene::CameraMode::ThirdPerson);
		else
			cam.set_mode(bud::scene::CameraMode::FreeFly);
	}
	prev_v = curr_v;

	if (auto* sm = engine->get_streaming_manager()) {
		sm->update(bud::math::vec3(cam.position.x, cam.position.y, cam.position.z));
	}

	if (input.is_key_down(bud::input::Key::W)) cam.process_keyboard(0, delta_time);
	if (input.is_key_down(bud::input::Key::S)) cam.process_keyboard(1, delta_time);
	if (input.is_key_down(bud::input::Key::A)) cam.process_keyboard(2, delta_time);
	if (input.is_key_down(bud::input::Key::D)) cam.process_keyboard(3, delta_time);

	if (input.is_gamepad_connected()) {
		float lx = input.get_gamepad_axis(bud::input::GamepadAxis::LeftX);
		float ly = input.get_gamepad_axis(bud::input::GamepadAxis::LeftY);
		float speed = cam.movement_speed * delta_time;
		if (cam.get_mode() == bud::scene::CameraMode::ThirdPerson) {
			cam.target_position += cam.right * lx * speed;
			cam.target_position += cam.front * (-ly) * speed;
		} else {
			cam.position += cam.right * lx * speed;
			cam.position += cam.front * (-ly) * speed;
		}

		float rx = input.get_gamepad_axis(bud::input::GamepadAxis::RightX);
		float ry = input.get_gamepad_axis(bud::input::GamepadAxis::RightY);
		if (rx != 0.0f || ry != 0.0f)
			cam.process_mouse_movement(rx * 100.0f * delta_time, ry * 100.0f * delta_time, true);
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
