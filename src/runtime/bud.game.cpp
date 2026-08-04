#include "src/runtime/bud.game.hpp"
#include <iostream>

namespace bud::game {

    void GameFramework::run(const AppConfig& config) {
        try {
            bud::print("[Framework] Starting Application: {}", config.window_title);
            
            // 1. Initialize Engine
            engine = std::make_unique<bud::engine::BudEngine>(config.to_engine_config());

            // 2. Lifecycle: on_init
            on_init(config);

            // 3. Main Loop
            engine->run([this](float dt) {
                on_update(dt);
            });

            // 4. Lifecycle: on_shutdown
            on_shutdown();

        } catch (const std::exception& e) {
            bud::eprint("[Framework] Fatal Hub Error: {}", e.what());
        }
    }

    void GameFramework::init_puppet(const AppConfig& config) {
        bud::print("[Framework] Initializing Puppet Environment: {}", config.window_title);
        engine = std::make_unique<bud::engine::BudEngine>(config.to_engine_config());
        on_init(config);
    }

    void GameFramework::step_puppet(float dt) {
        if (engine) {
            engine->step(dt, [this](float d) {
                on_update(d);
            });
        }
    }

}
