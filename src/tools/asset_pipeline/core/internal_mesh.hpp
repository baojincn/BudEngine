#pragma once

#include "raw_mesh.hpp"

namespace bud::asset_pipeline {

struct InternalSubmesh {
    std::string name;
    uint32_t material_index = 0;
    std::vector<bud::asset::Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct InternalMesh {
    std::string name;
    std::vector<InternalSubmesh> submeshes;
    std::vector<RawMaterial> materials;
    std::vector<std::string> textures;
    float aabb_min[3] = { 0.0f, 0.0f, 0.0f };
    float aabb_max[3] = { 0.0f, 0.0f, 0.0f };

    static InternalMesh from_raw_mesh(const RawMesh& raw);
};

} // namespace bud::asset_pipeline
