#include <print>
#include <exception>
#include <functional>

#include "src/io/bud.io.hpp"
#include "src/runtime/bud.engine.hpp"

class GameApp {
public:
	void init(bud::engine::BudEngine* engine_instance) {
		engine = engine_instance;

		std::println("[Game] App initialized. Loading Sponza...");

		auto asset_manager = engine->get_asset_manager();
		auto renderer = engine->get_renderer();
		auto scene = &engine->get_scene();

		auto callback = std::bind_front(&GameApp::on_sponza_loaded, this);

		asset_manager->load_mesh_async("data/cryteksponza/sponza.obj", callback);

		bud::graphics::RenderConfig config;
		config.directional_light_position = { 50.0f, 500.0f, 50.0f };
		config.directional_light_intensity = 3.0f;
		config.shadow_bias_constant = 0.005f;
		config.shadow_bias_slope = 1.25f;
		config.cache_shadows = false;
		config.ambient_strength = 0.4f;
		config.debug_cascades = false;

		renderer->set_config(config);
	}

	void update(float delta_time) {
		if (!engine) return;

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
		auto& scene = engine->get_scene();

		auto idx = renderer->upload_mesh(mesh);

		bud::scene::Entity entity;
		entity.mesh_index = idx;
		entity.transform = bud::math::scale(bud::math::mat4(1.0f), bud::math::vec3(1.0f));

		scene.entities.push_back(entity);
		std::println("[Game] Sponza loaded and spawned via Member Function!");
	}

private:
	bud::engine::BudEngine* engine = nullptr;
};


int main(int argc, char* argv[]) {
	try {
		bud::engine::BudEngine engine("Bud Engine - Triangle", 1920, 1080);

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
