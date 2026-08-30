
#include "triangle.hpp"
#include <string>


int main(int argc, char* argv[]) {
    bud::print("[TriangleApp] Main started.");
    try {
        bud::game::AppConfig config;
        config.window_title = "Bud Engine";
        config.scene_file = "Content/Scenes/suntemple_page_scene.json";

        // --scene <path> overrides the default scene file (used by the
        // streaming/format verification harness).
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--scene")
                config.scene_file = argv[i + 1];
        }

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
