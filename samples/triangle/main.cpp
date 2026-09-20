#include "triangle.hpp"
#include <string>
#include <cstdio>
#include <exception>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_NAME_FILTER", "__DISABLED__");
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_OP_TYPE_FILTER", "__DISABLED__");
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_DUMP_SHAPE_DATA", "0");
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_DUMP_INPUT_DATA", "0");
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_DUMP_OUTPUT_DATA", "0");
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_DUMP_STATISTICS_DATA", "0");
    SetEnvironmentVariableA("ORT_DEBUG_NODE_IO_DUMP_NODE_PLACEMENT", "0");
    _putenv("ORT_DEBUG_NODE_IO_NAME_FILTER=__DISABLED__");
    _putenv("ORT_DEBUG_NODE_IO_OP_TYPE_FILTER=__DISABLED__");
    _putenv("ORT_DEBUG_NODE_IO_DUMP_SHAPE_DATA=0");
    _putenv("ORT_DEBUG_NODE_IO_DUMP_INPUT_DATA=0");
    _putenv("ORT_DEBUG_NODE_IO_DUMP_OUTPUT_DATA=0");
    _putenv("ORT_DEBUG_NODE_IO_DUMP_STATISTICS_DATA=0");
    _putenv("ORT_DEBUG_NODE_IO_DUMP_NODE_PLACEMENT=0");
#endif
    std::set_terminate([]() {
        std::exception_ptr p = std::current_exception();
        if (p) {
            try {
                std::rethrow_exception(p);
            }
            catch (const std::exception& e) {
                std::fprintf(stderr, "[FATAL] std::terminate called with exception: %s\n", e.what());
            }
            catch (...) {
                std::fprintf(stderr, "[FATAL] std::terminate called with unknown exception\n");
            }
        }
        else {
            std::fprintf(stderr, "[FATAL] std::terminate called without active exception\n");
        }
        std::fflush(stderr);
        std::abort();
    });
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
        std::string policy_path;
        std::string policy_spec_path;
        bud::math::vec3 policy_command(0.0f);
        bool has_policy_command = false;
        bool companion_enabled = false;
        std::string companion_policy_path;
        std::string companion_asset_path;

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
            else if (arg == "--policy" && i + 1 < argc) {
                policy_path = argv[++i];
            }
            else if (arg == "--policy-spec" && i + 1 < argc) {
                policy_spec_path = argv[++i];
            }
            else if (arg == "--cmd" && i + 3 < argc) {
                // Read into named locals: argument evaluation order is unspecified, so three
                // pre-increments inside one call would not map to vx/vy/yaw reliably.
                const float command_x = std::stof(argv[i + 1]);
                const float command_y = std::stof(argv[i + 2]);
                const float command_yaw = std::stof(argv[i + 3]);
                i += 3;
                policy_command = bud::math::vec3(command_x, command_y, command_yaw);
                has_policy_command = true;
            }
            else if (arg == "--companion") {
                companion_enabled = true;
            }
            else if (arg == "--companion-policy" && i + 1 < argc) {
                companion_policy_path = argv[++i];
                companion_enabled = true;
            }
            else if (arg == "--companion-asset" && i + 1 < argc) {
                companion_asset_path = argv[++i];
                companion_enabled = true;
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
        if (!policy_spec_path.empty())
            app.set_policy(policy_path, policy_spec_path);
        if (has_policy_command)
            app.set_policy_command(policy_command);
        if (companion_enabled)
            app.set_companion_enabled(true, companion_asset_path, companion_policy_path);
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
