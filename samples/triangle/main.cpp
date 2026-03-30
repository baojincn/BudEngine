
#include "triangle.hpp"


int main(int argc, char* argv[]) {
    bud::print("[TriangleApp] Main started.");
    try {
        bud::game::AppConfig config;
        config.window_title = "Bud Engine";
        config.scene_file = "data/scenes/sponza_scene.json";

        const auto screen = bud::platform::get_current_screen_resolution();
        if (screen.width > 0 && screen.height > 0) {
            config.width = screen.width;
            config.height = screen.height;
        }

        TriangleApp app;
        app.run(config);

    }
    catch (const std::exception& e) {
        bud::eprint("Fatal Error: {}", e.what());
        return -1;
    }

    return 0;
}
