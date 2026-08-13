#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include "src/core/bud.asset.types.hpp"

namespace bud::tool {
    class AssetProcessor {
    public:
        // Processes a glTF file and exports it to the .budmesh format
        // max_vertices: maximum vertices per meshlet (default 64)
        // max_triangles: maximum triangles per meshlet (default 128)
        // cone_weight: weight parameter for meshlet generation bounds (default 0.5f)
        static bool process_gltf_to_budmesh(const std::string& input_path, const std::string& output_path,
                                            size_t max_vertices = 64,
                                            size_t max_triangles = 128,
                                            float cone_weight = 0.5f,
                                            size_t page_size = 0);

        static bool process_gltf_to_budmesh_json(const std::string& input_path, const std::string& output_path,
                                                  size_t max_vertices = 64, size_t max_triangles = 128,
                                                  float cone_weight = 0.5f, size_t page_size = 131040);
        // Processes a glTF file and exports it to the .budmesh (UE5-aligned
        // layout, magic "BNNT"; the file extension stays .budmesh) format:
        // cluster DAG + fixed 128KB pages + quantized positions + packed
        // attributes.
        static bool process_gltf_to_budnanite(const std::string& input_path, const std::string& output_path);
        // Validate shaders under a directory (compile with glslc if needed and run SPIR-V reflection)
        // If report_path is non-empty, writes a JSON report to that file
        // max_workers: if >0, limit parallel workers; if 0, tool will use env var or hardware_concurrency
        static bool validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path = "", unsigned int max_workers = 0);
    };
}
