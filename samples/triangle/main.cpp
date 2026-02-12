#include <print>
#include <exception>
#include <functional>
#include <unordered_map>

#include "src/io/bud.io.hpp"
#include "src/runtime/bud.engine.hpp"

class GameApp {
public:
	void init(bud::engine::BudEngine* engine_instance) {
		engine = engine_instance;

		std::println("[Game] App initialized. Loading Sponza...");

		auto asset_manager = engine->get_asset_manager();
		auto renderer = engine->get_renderer();

		auto callback = std::bind_front(&GameApp::on_sponza_loaded, this);

		asset_manager->load_mesh_async("data/cryteksponza/sponza.obj", callback);

		bud::graphics::RenderConfig config;
		config.shadow_bias_constant = 0.005f;
		config.shadow_bias_slope = 1.25f;
		config.cache_shadows = true;
		config.cascade_count = 4;
		config.cascade_split_lambda = 0.5;
		config.debug_cascades = false;

		renderer->set_config(config);

		auto& scene = engine->get_scene();
		scene.directional_light.direction = { 50.0f, 500.0f, 50.0f };
		scene.directional_light.intensity = 3.0f;
		scene.ambient_strength = 0.1f;

		scene.main_camera = bud::scene::Camera(bud::math::vec3(0.0f, 100.0f, 0.0f));
		scene.main_camera.movement_speed = 70.0f;
	}

	void update(float delta_time) {
		if (!engine) return;

		// 在主线程安全地将新实体加入场景
		{
			std::lock_guard lock(entity_mutex);
			if (!pending_entities.empty()) {
				auto& scene = engine->get_scene();
				for (const auto& e : pending_entities) {
					scene.entities.push_back(e);
				}

				pending_entities.clear();
			}
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

	void shutdown() {
		std::println("[Game] App shutting down.");
	}

private:
	void on_sponza_loaded(bud::io::MeshData mesh) {
		if (!engine)
			return;

		auto renderer = engine->get_renderer();

		if (mesh.subsets.empty()) {
			auto mesh_handle = renderer->upload_mesh(mesh);

			if (!mesh_handle.is_valid()) {
				std::println("[Game] Mesh upload failed.");
				return;
			}

			bud::scene::Entity entity;
			entity.mesh_index = mesh_handle.mesh_id;
			entity.material_index = mesh_handle.material_id;
			entity.transform = bud::math::scale(bud::math::mat4(1.0f), bud::math::vec3(1.0f));
			entity.is_static = true;

			{
				std::lock_guard lock(entity_mutex);
				pending_entities.push_back(entity);
			}

			std::println("[Game] Sponza loaded and spawned via Member Function!");
			return;
		}

		for (const auto& subset : mesh.subsets) {
			bud::io::MeshData sub_mesh;
			sub_mesh.subsets.reserve(1);

			if (subset.material_index < mesh.texture_paths.size()) {
				sub_mesh.texture_paths.push_back(mesh.texture_paths[subset.material_index]);
			}
			else {
				sub_mesh.texture_paths.push_back("data/textures/default.png");
			}

			std::unordered_map<uint32_t, uint32_t> index_map;
			index_map.reserve(subset.index_count);
			sub_mesh.indices.reserve(subset.index_count);

			for (uint32_t i = 0; i < subset.index_count; ++i) {
				uint32_t old_index = mesh.indices[subset.index_start + i];
				auto [it, inserted] = index_map.emplace(old_index, static_cast<uint32_t>(sub_mesh.vertices.size()));

				if (inserted) {
					sub_mesh.vertices.push_back(mesh.vertices[old_index]);
				}

				sub_mesh.indices.push_back(it->second);
			}

			bud::io::MeshSubset sub_subset;
			sub_subset.index_start = 0;
			sub_subset.index_count = static_cast<uint32_t>(sub_mesh.indices.size());
			sub_subset.material_index = 0;
			sub_mesh.subsets.push_back(sub_subset);

			auto mesh_handle = renderer->upload_mesh(sub_mesh);
			if (!mesh_handle.is_valid()) {
				std::println("[Game] Sub-mesh upload failed.");
				continue;
			}

			bud::scene::Entity entity;
			entity.mesh_index = mesh_handle.mesh_id;
			entity.material_index = mesh_handle.material_id;
			entity.transform = bud::math::scale(bud::math::mat4(1.0f), bud::math::vec3(1.0f));
			entity.is_static = true;

			{
				std::lock_guard lock(entity_mutex);
				pending_entities.push_back(entity);
			}
		}

		std::println("[Game] Sponza loaded and spawned as per-subset entities.");
	}

private:
	bud::engine::BudEngine* engine = nullptr;
	std::mutex entity_mutex;
	std::vector<bud::scene::Entity> pending_entities;
};


int main(int argc, char* argv[]) {
	try {
		bud::graphics::EngineConfig config;
		config.name = "Bud Engine - Triangle";

		const auto screen = bud::platform::get_current_screen_resolution();
		if (screen.width > 0 && screen.height > 0) {
			config.width = screen.width;
			config.height = screen.height;
		}

		bud::engine::BudEngine engine(config);

		GameApp app_instance;

		app_instance.init(&engine);

		engine.run([&](float delta_time) {
			app_instance.update(delta_time);
		});

		app_instance.shutdown();

	}
	catch (const std::exception& e) {
		std::println(stderr, "Fatal Error: {}", e.what());
		return -1;
	}

    std::println("Engine shutdown gracefully.");
	return 0;
}
