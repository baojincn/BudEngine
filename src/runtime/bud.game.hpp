#pragma once

#include <string>
#include <vector>
#include <memory>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/runtime/bud.engine.hpp"

namespace bud::game {

	struct AppConfig {
		std::string scene_file = "";
		std::string window_title = "Bud Engine Application";
		uint32_t width = 1280;
		uint32_t height = 720;

		bud::graphics::EngineConfig to_engine_config() const {
			bud::graphics::EngineConfig config;
			config.name = window_title;
			config.width = width;
			config.height = height;
			return config;
		}
	};

	class GameFramework {
	public:
		virtual ~GameFramework() = default;

		// Entry point: Initializes engine, calls on_init, and starts the loop
		void run(const AppConfig& config);

		// Lifecycle hooks
		virtual void on_init(const AppConfig& config) = 0;
		virtual void on_update(float dt) = 0;
		virtual void on_shutdown() = 0;

	protected:
		bud::engine::BudEngine* get_engine() { return engine.get(); }

	private:
		std::unique_ptr<bud::engine::BudEngine> engine;
	};

}
