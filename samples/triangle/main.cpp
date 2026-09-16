#include "triangle.hpp"
#include <string>

int main(int argc, char* argv[]) {
    bud::print("[TriangleApp] Main started.");
    try {
        bud::game::AppConfig config;
        config.window_title = "Bud Engine";
        config.scene_file = "Content/Scenes/sponza_page_scene.json";

        // Sponza courtyard stone floor visual elevation is at Y = -0.0251m.
        // Align MuJoCo physical ground plane to eliminate floating gap between foot soles and stone tiles.
        constexpr float k_sponza_ground_y = -0.0251f;
        config.ground_plane_height = k_sponza_ground_y;

        bool custom_resolution = false;

        float test_duration = 0.0f;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--scene" && i + 1 < argc) {
                config.scene_file = argv[++i];
                if (config.scene_file.find("sponza") == std::string::npos)
                    config.ground_plane_height = 0.0f;
            }
            else if (arg == "--ground_y" && i + 1 < argc) {
                config.ground_plane_height = std::stof(argv[++i]);
            }
            else if (arg == "--width" && i + 1 < argc) {
                config.width = static_cast<uint32_t>(std::stoul(argv[++i]));
                custom_resolution = true;
            }
            else if (arg == "--height" && i + 1 < argc) {
                config.height = static_cast<uint32_t>(std::stoul(argv[++i]));
                custom_resolution = true;
            }
            else if (arg == "--mode" && i + 1 < argc) {
                std::string mode_str = argv[++i];
                if (mode_str == "sim" || mode_str == "simulation")
                    config.mode = bud::graphics::EngineMode::Simulation;
                else
                    config.mode = bud::graphics::EngineMode::Game;
            }
            else if (arg == "--backend" && i + 1 < argc) {
                std::string b_str = argv[++i];
                if (b_str == "mujoco")
                    config.mode = bud::graphics::EngineMode::Simulation;
                else if (b_str == "jolt")
                    config.mode = bud::graphics::EngineMode::Game;
            }
            else if (arg == "--duration" && i + 1 < argc) {
                test_duration = std::stof(argv[++i]);
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
        if (test_duration > 0.0f)
            app.set_test_duration(test_duration);
        app.run(config);

        if (app.has_test_failed())
            return -1;
    }
    catch (const std::exception& e) {
        bud::eprint("Fatal Error: {}", e.what());
        return -1;
    }

    return 0;
}
