#pragma once

#include <string>

namespace bud::asset_pipeline {

struct MeshBuildOptions {
    bool enable_virtual_geometry = true;
    bool use_cache = true;
    bool dump_text = false;
    std::string cache_root = "AssetCache";
};

class MeshBuilder {
public:
    static bool build(const std::string& input_path, const std::string& output_path, const MeshBuildOptions& options = {});
};

} // namespace bud::asset_pipeline
