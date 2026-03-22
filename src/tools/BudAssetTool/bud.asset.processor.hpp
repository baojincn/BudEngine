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
        // Validate shaders under a directory (compile with glslc if needed and run SPIR-V reflection)
        // If report_path is non-empty, writes a JSON report to that file
        // max_workers: if >0, limit parallel workers; if 0, tool will use env var or hardware_concurrency
        static bool validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path = "", unsigned int max_workers = 0);
    };
}
