#pragma once

#include "raw_mesh.hpp"
#include <string>
#include <vector>
#include <optional>

namespace bud::asset_pipeline {

struct RawSceneMesh {
    uint32_t mesh_index = 0;
    std::string name;
    std::string relative_dir;
    RawMesh mesh;
    bool is_translucent = false;
};

struct RawSceneInstance {
    std::string name;
    uint32_t mesh_index = 0;
    std::string mesh_name;
    std::string relative_dir;
    float transform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    uint32_t material_override = 0xFFFFFFFF;
    bool is_translucent = false;
};

struct RawScene {
    std::string name;
    std::string source_path;
    std::vector<RawSceneMesh> meshes;
    std::vector<RawSceneInstance> instances;
    std::vector<RawMaterial> materials;
    std::vector<std::string> textures;

    float aabb_min[3] = { 0.0f, 0.0f, 0.0f };
    float aabb_max[3] = { 0.0f, 0.0f, 0.0f };

    void compute_bounds() {
        if (meshes.empty()) return;
        aabb_min[0] = aabb_min[1] = aabb_min[2] = 1e30f;
        aabb_max[0] = aabb_max[1] = aabb_max[2] = -1e30f;

        for (const auto& sm : meshes) {
            for (const auto& v : sm.mesh.vertices) {
                for (int c = 0; c < 3; ++c) {
                    if (v.position[c] < aabb_min[c]) aabb_min[c] = v.position[c];
                    if (v.position[c] > aabb_max[c]) aabb_max[c] = v.position[c];
                }
            }
        }
    }
};

} // namespace bud::asset_pipeline
