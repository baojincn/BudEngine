#pragma once

#include <pybind11/pybind11.h>
#include <memory>
#include <atomic>
#include "src/runtime/bud.game.hpp"

namespace bud::application {

	class GymEnvApp : public bud::game::GameFramework {
	public:
		GymEnvApp();
		~GymEnvApp() override;

		void on_init(const bud::game::AppConfig& config) override;
		void on_update(float delta_time) override;
		void on_shutdown() override;

		bool is_fully_loaded() const override;

		// RL-specific step: advances frame and returns RGBA8 pixel bytes
		pybind11::bytes step(float dt);

		// RL reset: resets physics/scene state
		void reset();

	private:
		std::shared_ptr<std::atomic<int>> pending_mesh_loads;
	};

} // namespace bud::application
