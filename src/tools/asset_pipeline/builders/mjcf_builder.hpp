#pragma once

#include "src/robots/bud.robot.types.hpp"
#include <string>

namespace bud::asset_pipeline {

struct MjcfBuildOptions {
    int max_convex_vertices = 128;
    float scale = 0.0f;
    bool dump_json = true;
    bool use_cache = true;
};

class MjcfBuilder {
public:
    static bool build_mjcf(
        const std::string& mjcf_path,
        const std::string& output_robot_dir,
        const MjcfBuildOptions& options = {}
    );
};

} // namespace bud::asset_pipeline
