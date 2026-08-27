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

using namespace bud::game;


bool TriangleApp::is_fully_loaded() const {
	return true; // Page streaming is async, start rendering immediately
}

void TriangleApp::on_init(const AppConfig& config) {
	bud::print("[TriangleApp] Initialized. Loading scene: {}", config.scene_file);

	auto engine = get_engine();
	auto asset_manager = engine->get_asset_manager();
	auto renderer = engine->get_renderer();

	streaming_manager = std::make_unique<bud::streaming::StreamingManager>(
		asset_manager, &renderer->get_gpu_scene(), renderer, renderer->get_rhi());

	renderer->set_streaming_manager(streaming_manager.get());

	streaming_manager->set_asset_registered_callback([engine](uint32_t mesh_id, const bud::math::AABB&, uint32_t root_group_index, uint32_t base_virtual_page) {
		auto& s = engine->get_scene();
		bud::scene::Entity e;
		e.asset_path = "[asset_streaming]";
		e.mesh_index = mesh_id;
		e.material_index = 0;
		e.is_active = true;
		e.is_static = true;
		e.transform = glm::mat4(1.0f);
		e.root_group_index = root_group_index;
		e.base_virtual_page = base_virtual_page;
		s.entities.push_back(std::move(e));
		//bud::print("[TriangleApp] Asset entity added: mesh_id={} total_entities={}", mesh_id, s.entities.size());
	});

	// 1. Initial Render Config
	bud::graphics::RenderConfig render_config;
	render_config.shadow_bias_constant = 0.005f;
	render_config.shadow_bias_slope = 1.25f;
	render_config.cache_shadows = true;
	render_config.cascade_count = 4;
	render_config.cascade_split_lambda = 0.5;
	render_config.debug_cascades = false;
	// CPU-driven path: page-backed streaming is validated via the CPU indirect
	// path (GPU-driven meshlet rendering would need per-page meshlet GPU data
	// that streaming does not upload yet).
	render_config.enable_virtual_geometry = true;
	renderer->set_config(render_config);

	// 2. Load Scene Data-Driven
	if (!config.scene_file.empty()) {
		asset_manager->load_json_async(config.scene_file, [this, engine, renderer, asset_manager](const nlohmann::json& j) {
			try {
				auto& scene = engine->get_scene();
				scene = j.get<bud::scene::Scene>();

				bud::print("[TriangleApp] Scene file parsed. Entities found: {}", scene.entities.size());

				auto is_vg_asset = [](const std::string& p) {
					return p.ends_with(".budasset") || p.ends_with(".budmesh");
				};

				if (streaming_manager) {
					streaming_manager->set_asset_registered_callback([this, engine, renderer](uint32_t mesh_id, const bud::math::AABB& aabb, uint32_t root_group_index, uint32_t base_virtual_page) {
						renderer->register_mesh_bounds(mesh_id, aabb);
						auto& s = engine->get_scene();
						for (auto& ent : s.entities) {
							if (!ent.asset_path.empty() && (ent.asset_path.ends_with(".budasset") || ent.asset_path.ends_with(".budmesh"))) {
								ent.mesh_index = mesh_id;
								ent.root_group_index = root_group_index;
								ent.base_virtual_page = base_virtual_page;
							}
						}
						bud::print("[TriangleApp] Virtual Geometry mesh registered: mesh_id={}, root_group={}, base_page={}", mesh_id, root_group_index, base_virtual_page);
					});
				}

				// Route Virtual Geometry assets (.budasset / .budmesh) through GPU page streaming;
				// everything else loads as a traditional dynamic mesh below.
				for (auto& e : scene.entities) {
					if (!e.asset_path.empty() && is_vg_asset(e.asset_path)) {
						if (streaming_manager)
							streaming_manager->register_virtual_geometry_async(e.asset_path);
						e.mesh_index = bud::asset::INVALID_INDEX;
					}
				}

				int count = 0;
				for (auto& e : scene.entities)
					if (!e.asset_path.empty() && !is_vg_asset(e.asset_path))
						++count;

				pending_mesh_loads->store(count);

				if (count == 0) {
					bud::print("[TriangleApp] init finished");
					return;
				}

				for (size_t i = 0; i < scene.entities.size(); ++i) {
					const auto asset_path = scene.entities[i].asset_path;
					if (asset_path.empty() || is_vg_asset(asset_path)) continue;

					asset_manager->load_mesh_async(asset_path, [this, engine, renderer, pending_mesh = pending_mesh_loads, asset_path, i](bud::io::MeshData mesh) mutable {
						auto mesh_handle = renderer->upload_mesh(mesh);

						if (mesh_handle.is_valid()) {
							auto& s = engine->get_scene();

							if (i < s.entities.size() && s.entities[i].asset_path == asset_path) {
								s.entities[i].mesh_index = mesh_handle.mesh_id;
								s.entities[i].material_index = mesh_handle.material_id;
							}
							else {
								// Fallback: find by asset_path
								for (auto& ent : s.entities) {
									if (ent.asset_path == asset_path) {
										ent.mesh_index = mesh_handle.mesh_id;
										ent.material_index = mesh_handle.material_id;
										break;
									}
								}
							}
							bud::print("[TriangleApp] Loaded mesh: {}", asset_path);
						}

						if (pending_mesh->fetch_sub(1) == 1) {
							bud::print("[TriangleApp] init finished");
						}
						});
				}
			}
			catch (const std::exception& e) {
				bud::eprint("[TriangleApp] CRITICAL EXCEPTION during JSON parsing: {}", e.what());
			}
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

	if (streaming_manager) {
		auto& cam = engine->get_scene().main_camera;
		streaming_manager->update(bud::math::vec3(cam.position.x, cam.position.y, cam.position.z));
	}

	auto& input = bud::input::Input::get();
	auto& scene = engine->get_scene();
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

void TriangleApp::on_shutdown() {
	bud::print("[TriangleApp] Shutting down.");
	streaming_manager.reset();
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
