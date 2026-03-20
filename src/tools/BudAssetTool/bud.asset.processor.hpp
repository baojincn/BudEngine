#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include "src/core/bud.asset.types.hpp"

namespace bud::tool {
    class AssetProcessor {
    public:
        // Processes a glTF file and exports it to the .budmesh format
        static bool process_gltf_to_budmesh(const std::string& input_path, const std::string& output_path);
    };
}
