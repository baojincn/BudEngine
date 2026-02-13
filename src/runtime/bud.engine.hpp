#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <limits>

#include "src/io/bud.io.hpp"
#include "src/core/bud.core.hpp"
#include "src/core/bud.math.hpp"
#include "src/runtime/bud.input.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/platform/bud.platform.hpp"

#include "src/graphics/bud.graphics.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"


namespace bud::engine {

	enum class EngineMode {
		TASK_BASED,
		THREAD_BASED,
		MIXED
	};

	class BudEngine {
	public:

		using GameLogic = std::function<void(float)>;

		BudEngine(const bud::graphics::EngineConfig config);
		~BudEngine();

		void run(GameLogic perform_game_logic);

		auto* get_asset_manager() { return asset_manager.get(); }
		auto* get_renderer() { return renderer.get(); }
		auto& get_scene() { return scene; }

		auto* get_task_scheduler() { return task_scheduler.get(); }

		auto& get_engine_config() const { return engine_config; }

	private:
		void handle_events();

		void extract_scene_data(bud::graphics::RenderScene& render_scene);

		void sync_game_to_rendering(uint32_t render_scene_index);

		void perform_rendering(float delta_time, uint32_t render_scene_index);

	private:

		double accumulator = 0.0;

		uint32_t current_write_index = 0;

		std::atomic<uint32_t> last_committed_index = 0;

		static constexpr uint32_t invalid_render_index = std::numeric_limits<uint32_t>::max();
		std::atomic<uint32_t> render_inflight_index = invalid_render_index;

		bud::threading::Counter render_task_counter;

		std::unique_ptr<bud::platform::Window> window;

		int last_width = 0;
		int last_height = 0;

		std::unique_ptr<bud::threading::TaskScheduler> task_scheduler;
		std::unique_ptr<bud::graphics::RHI> rhi;
		std::unique_ptr<bud::io::AssetManager> asset_manager;
		std::unique_ptr<bud::graphics::Renderer> renderer;

		// 场景数据
		bud::scene::Scene scene;
		std::vector<bud::graphics::RenderScene> render_scenes;
		const bud::graphics::EngineConfig engine_config;

		// 渲染配置
		float far_plane{ 5000.0f };
		float near_plane{ 1.0f };

	};
}
