module;

#include <string>
#include <memory>

export module bud.engine;

import bud.core;
import bud.math;
import bud.dod;
import bud.threading;
import bud.platform;
import bud.graphics.rhi;

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
        void perform_frame_logic();

    private:
        bool running_ = true;

        std::unique_ptr<bud::platform::Window> window_;

        std::unique_ptr<bud::threading::TaskScheduler> task_scheduler_;
        bud::threading::Counter frame_counter_{ 0 };

		std::unique_ptr<bud::graphics::RHI> rhi_;

        bud::dod::Registry<bud::math::float3, float> registry_;
    };
}
