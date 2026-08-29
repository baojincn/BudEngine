#pragma once

#include "mesh_builder.hpp"
#include "../core/raw_mesh.hpp"
#include "../core/raw_scene.hpp"
#include <string>

namespace bud::asset_pipeline {

struct CascadeBuildOptions {
    bool dump_text = false;
    bool use_cache = true;
    float scale = 1.0f;
    std::string cache_root = "Cache";
    std::string scene_output_dir = "Content/Scenes";
};

class CascadeBuilder {
public:
    static bool build_package_from_raw(
        const RawMesh& raw_mesh,
        const std::string& output_package_dir,
        const CascadeBuildOptions& options = {}
    );

    static bool build_package(
        const std::string& input_mesh_path,
        const std::string& output_package_dir,
        const CascadeBuildOptions& options = {}
    );

    static bool build_scene_package(
        const RawScene& scene,
        const std::string& output_package_dir,
        const CascadeBuildOptions& options = {}
    );
};

} // namespace bud::asset_pipeline
