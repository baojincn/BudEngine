#pragma once

#include "mesh_builder.hpp"
#include "../core/raw_mesh.hpp"
#include "../core/raw_scene.hpp"
#include <string>

namespace bud::asset_pipeline {

enum class SceneImportMode : uint32_t {
    None = 0,       // Only cook assets into Content package (default)
    Append = 1      // Append imported instances into an existing target scene file
};

struct CascadeBuildOptions {
    bool dump_text = false;
    bool use_cache = true;
    float scale = 1.0f;
    std::string cache_root = "Cache";
    SceneImportMode scene_mode = SceneImportMode::None;
    std::string target_scene_path = "";
    std::string entity_prefix = "";
	std::string default_scene_dir = "Content/Scenes";
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
