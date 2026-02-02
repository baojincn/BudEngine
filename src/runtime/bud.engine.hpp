#pragma once

#include <string>
#include <memory>
#include <functional>

#include "src/io/bud.io.hpp"
#include "src/core/bud.core.hpp"
#include "src/core/bud.math.hpp"
#include "src/runtime/bud.input.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/platform/bud.platform.hpp"

#include "src/graphics/bud.graphics.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"


namespace bud::engine {

	enum class EngineMode {
		TASK_BASED,
		THREAD_BASED,
		MIXED
	};

	class BudEngine {
	public:

		using TickCallback = std::function<void(float)>;

		BudEngine(const std::string& window_title, int width, int height);
		~BudEngine();

		void run(TickCallback tick);

		auto* get_asset_manager() { return asset_manager.get(); }
		auto* get_renderer() { return renderer.get(); }
		auto& get_scene() { return scene; }

	private:
		void handle_events();

		void perform_rendering(float delta_time);

	private:
		std::unique_ptr<bud::platform::Window> window;

		std::unique_ptr<bud::threading::TaskScheduler> task_scheduler;
		std::unique_ptr<bud::io::AssetManager> asset_manager;
		std::unique_ptr<bud::graphics::RHI> rhi;
		std::unique_ptr<bud::graphics::Renderer> renderer;

		// 场景数据
		bud::scene::Scene scene;

		// 渲染配置
		float aspect_ratio{ 16.0f / 9.0f };
		float far_plane{ 5000.0f };
		float near_plane{ 1.0f };

		bool running = true;
	};
}
