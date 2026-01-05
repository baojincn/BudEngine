module;

#include <string>
#include <memory>

export module bud.engine;

import bud.core;
import bud.math;
import bud.dod;
import bud.threading;
import bud.platform;
import bud.graphics;

export namespace bud::engine {

    enum class EngineMode {
        TASK_BASED,
        THREAD_BASED,
        MIXED
    };

    class BudEngine {
    public:
        BudEngine(const std::string& window_title, int width, int height);
        ~BudEngine();

        void run();

    private:
		void handle_events();
        void perform_frame_logic(float delta_time);
		void perform_rendering();

    private:

        std::unique_ptr<bud::platform::Window> window_;

        std::unique_ptr<bud::threading::TaskScheduler> task_scheduler_;
        bud::threading::Counter frame_counter_{ 0 };

		std::unique_ptr<bud::graphics::RHI> rhi_;

		bud::graphics::Camera camera_;

		float aspect_ratio_{ 16.0f / 9.0f };
		float far_plane_{ 4000.0f };
		float near_plane_{ 0.1f };

        bool running_ = true;
    };
}
