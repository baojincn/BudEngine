module;

#include <string>
#include <memory>
#include <functional>

export module bud.engine;

import bud.io;
import bud.core;
import bud.math;
import bud.input;
import bud.scene;
import bud.threading;
import bud.platform;

import bud.graphics;
import bud.graphics.renderer;


export namespace bud::engine {

	export enum class EngineMode {
		TASK_BASED,
		THREAD_BASED,
		MIXED
	};

	export class BudEngine {
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
		void perform_frame_logic(float delta_time);
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
		float far_plane{ 4000.0f };
		float near_plane{ 0.1f };

		bool running = true;
	};
}
