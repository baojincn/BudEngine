#include "triangle.hpp"
#include <string>

int main(int argc, char* argv[]) {
    bud::print("[TriangleApp] Main started.");
    try {
        bud::game::AppConfig config;
        config.window_title = "Bud Engine";
        config.scene_file = "Content/Scenes/sponza_page_scene.json";

        bool custom_resolution = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--scene" && i + 1 < argc)
                config.scene_file = argv[++i];
            else if (arg == "--width" && i + 1 < argc) {
                config.width = static_cast<uint32_t>(std::stoul(argv[++i]));
                custom_resolution = true;
            }
            else if (arg == "--height" && i + 1 < argc) {
                config.height = static_cast<uint32_t>(std::stoul(argv[++i]));
                custom_resolution = true;
            }
        }

        if (!custom_resolution) {
            const auto screen = bud::platform::get_current_screen_resolution();
            if (screen.width > 0 && screen.height > 0) {
                config.width = screen.width;
                config.height = screen.height;
            }
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
