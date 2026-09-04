#pragma once

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <limits>

#include "src/io/bud.io.hpp"
#include "src/core/bud.core.hpp"
#include "src/core/bud.logger.hpp"
#include "src/core/bud.math.hpp"
#include "src/runtime/bud.input.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/threading/bud.threading.hpp"
#include "src/platform/bud.platform.hpp"

#include "src/graphics/bud.graphics.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"
#include "src/streaming/bud.streaming.manager.hpp"
#include "src/runtime/bud.camera_sequencer.hpp"
#include "src/input/bud.input.manager.hpp"


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
		void step(float fixed_dt, GameLogic perform_game_logic);

		bud::io::AssetManager* get_asset_manager() { return asset_manager.get(); }
		bud::graphics::Renderer* get_renderer() { return renderer.get(); }
		bud::streaming::StreamingManager* get_streaming_manager() { return streaming_manager.get(); }
		bud::scene::Scene& get_scene() { return scene; }

		// Data-Driven Scene & Asset Loader
		bool load_scene_async(const std::string& scene_path, std::function<void()> on_finished = nullptr);
		void load_scene_resources_async(std::function<void()> on_finished = nullptr);

		const void* get_readback_pixels() const { return renderer->get_readback_pixels(); }

		// Returns true while the camera sequencer is actively playing back a replay.
		// Use this in on_update() to suppress camera input during playback.
		bool is_replay_active() const { return camera_sequencer.is_playing(); }

		auto* get_task_scheduler() { return task_scheduler.get(); }

		auto& get_engine_config() const { return engine_config; }

	private:
		void handle_events();

		void extract_render_scene_data(bud::graphics::RenderScene& render_scene);

		void prepare_render_scene(uint32_t render_scene_index);

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
		std::unique_ptr<bud::input::InputManager> input_manager;
		std::unique_ptr<bud::Logger> logger;
		std::unique_ptr<bud::graphics::RHI> rhi;
		std::unique_ptr<bud::io::AssetManager> asset_manager;
		std::unique_ptr<bud::graphics::Renderer> renderer;
		std::unique_ptr<bud::streaming::StreamingManager> streaming_manager;
        std::unique_ptr<bud::io::VirtualFileSystem> virtual_file_system;

		// 场景数据
		bud::scene::Scene scene;
		std::vector<bud::graphics::RenderScene> render_scenes;
		const bud::graphics::EngineConfig engine_config;

		// 摄像机序列器
		bud::scene::CameraSequencer camera_sequencer;

		// 渲染配置
		float far_plane{ 500.0f * bud::core::units::m };  // 500.0 m
		float near_plane{ 0.01f * bud::core::units::m };  // 0.01 m (1 cm)

		bool show_debug_stats = true;

		bud::math::mat4 last_view_proj_matrix = bud::math::mat4(1.0f);
		bool has_last_view_proj = false;
		std::string imgui_ini_path;
	};
}
