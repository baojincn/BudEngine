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
		bool is_puppet_mode = false;
		bool is_headless = false;

		bud::graphics::EngineConfig to_engine_config() const {
			bud::graphics::EngineConfig config;
			config.name = window_title;
			config.width = width;
			config.height = height;
			config.is_puppet_mode = is_puppet_mode;
			config.is_headless = is_headless;
			return config;
		}
	};

	class GameFramework {
	public:
		virtual ~GameFramework() = default;

		// Entry point for continuous app (standard)
		void run(const AppConfig& config);

		// Entry points for discrete RL environments (puppet)
		void init_puppet(const AppConfig& config);
		void step_puppet(float dt);

		// Lifecycle hooks
		virtual void on_init(const AppConfig& config) = 0;
		virtual void on_update(float dt) = 0;
		virtual void on_shutdown() = 0;

		// Async resource load blocking
		virtual bool is_fully_loaded() const { return true; }

	protected:
		bud::engine::BudEngine* get_engine() { return engine.get(); }

	private:
		std::unique_ptr<bud::engine::BudEngine> engine;
	};

}
