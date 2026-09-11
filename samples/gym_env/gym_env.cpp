#include "gym_env.hpp"
#include <cstring>
#include "src/core/bud.core.hpp"
#include "src/io/bud.io.hpp"
#include "src/runtime/bud.engine.hpp"
#include "src/runtime/bud.scene.io.hpp"

namespace bud::application {

	GymEnvApp::GymEnvApp() {
		pending_mesh_loads = std::make_shared<std::atomic<int>>(1);
	}

	GymEnvApp::~GymEnvApp() = default;

	bool GymEnvApp::is_fully_loaded() const {
		return true;
	}

	void GymEnvApp::on_init(const bud::game::AppConfig& config) {
		bud::print("[GymEnvApp] Initialized. Loading scene: {}", config.scene_file);

		auto* engine = get_engine();
		if (!engine)
			return;

		auto* renderer = engine->get_renderer();
		if (!renderer)
			return;

		// 1. Initial Render Config for Headless RL
		bud::graphics::RenderConfig render_config;
		render_config.shadow_bias_constant = 2.0f;
		render_config.shadow_bias_slope = 1.75f;
		render_config.shadow_bias_clamp = 0.0f;
		render_config.shadow_normal_offset_texels = 1.0f;
		render_config.shadow_receiver_bias_texels = 1.5f;
		render_config.cascade_count = 4;
		render_config.cascade_split_lambda = 0.5f;
		render_config.debug_cascades = false;
		render_config.enable_virtual_geometry = true;
		render_config.enable_mesh_shader = true;
		renderer->set_config(render_config);

		// 2. Load Scene asynchronously
		if (!config.scene_file.empty()) {
			engine->load_scene_async(config.scene_file, [this, engine, renderer]() {
				auto& scene = engine->get_scene();
				if (auto* sm = engine->get_streaming_manager())
					sm->set_unload_radius(scene.streaming_unload_radius);

				auto cur_cfg = renderer->get_config();
				cur_cfg.lod_error_threshold_px = scene.lod_error_threshold_px;
				renderer->set_config(cur_cfg);

				pending_mesh_loads->store(0);
				bud::print("[GymEnvApp] Scene loading completed.");
			});
		}
		else {
			pending_mesh_loads->store(0);
			bud::print("[GymEnvApp] Empty scene initialized.");
		}
	}

	void GymEnvApp::on_update([[maybe_unused]] float delta_time) {
		// Environment stepping updates are handled in step()
	}

	void GymEnvApp::on_shutdown() {
		bud::print("[GymEnvApp] Shutting down.");
	}

	pybind11::bytes GymEnvApp::step(float dt) {
		step_puppet(dt);

		auto* engine = get_engine();
		const void* pixels = engine ? engine->get_readback_pixels() : nullptr;
		if (!pixels)
			return pybind11::bytes();

		const auto& engine_config = engine->get_engine_config();
		constexpr uint64_t bytes_per_pixel = 4;
		const uint64_t byte_count = static_cast<uint64_t>(engine_config.width) * engine_config.height * bytes_per_pixel;
		return pybind11::bytes(reinterpret_cast<const char*>(pixels), byte_count);
	}

	void GymEnvApp::reset() {
		auto* engine = get_engine();
		if (!engine)
			return;

		bud::print("[GymEnvApp] Resetting environment state.");
		// Future: Reset cloth particle positions, velocities, character controller, and domain randomization
	}

} // namespace bud::application
