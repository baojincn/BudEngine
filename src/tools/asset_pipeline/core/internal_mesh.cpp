#include "internal_mesh.hpp"
#include <unordered_map>
#include <cstring>

namespace bud::asset_pipeline {

InternalMesh InternalMesh::from_raw_mesh(const RawMesh& raw) {
    InternalMesh mesh;
    mesh.name = raw.source_path;
    mesh.materials = raw.materials;
    mesh.textures = raw.textures;
    std::memcpy(mesh.aabb_min, raw.aabb_min, sizeof(float) * 3);
    std::memcpy(mesh.aabb_max, raw.aabb_max, sizeof(float) * 3);

    if (raw.submeshes.empty()) {
        // Single submesh fallback
        InternalSubmesh sm;
        sm.name = "default";
        sm.material_index = 0;
        sm.vertices.resize(raw.vertices.size());
        for (size_t i = 0; i < raw.vertices.size(); ++i) {
            std::memcpy(sm.vertices[i].position, raw.vertices[i].position, sizeof(float) * 3);
            std::memcpy(sm.vertices[i].normal, raw.vertices[i].normal, sizeof(float) * 3);
            std::memcpy(sm.vertices[i].uv, raw.vertices[i].uv, sizeof(float) * 2);
            std::memcpy(sm.vertices[i].tangent, raw.vertices[i].tangent, sizeof(float) * 4);
        }
        sm.indices = raw.indices;
        if (!sm.vertices.empty() && !sm.indices.empty())
            mesh.submeshes.push_back(std::move(sm));
        return mesh;
    }

    for (const auto& raw_sm : raw.submeshes) {
        if (raw_sm.index_count == 0)
            continue;

        InternalSubmesh sm;
        sm.name = raw_sm.name;
        sm.material_index = raw_sm.material_index;

        std::unordered_map<uint32_t, uint32_t> remap;
        sm.indices.reserve(raw_sm.index_count);

        for (uint32_t i = 0; i < raw_sm.index_count; ++i) {
            uint32_t orig_idx = raw.indices[raw_sm.index_offset + i];
            auto it = remap.find(orig_idx);
            if (it != remap.end()) {
                sm.indices.push_back(it->second);
            } else {
                uint32_t new_idx = static_cast<uint32_t>(sm.vertices.size());
                remap[orig_idx] = new_idx;
                sm.indices.push_back(new_idx);

                bud::asset::Vertex v{};
                std::memcpy(v.position, raw.vertices[orig_idx].position, sizeof(float) * 3);
                std::memcpy(v.normal, raw.vertices[orig_idx].normal, sizeof(float) * 3);
                std::memcpy(v.uv, raw.vertices[orig_idx].uv, sizeof(float) * 2);
                std::memcpy(v.tangent, raw.vertices[orig_idx].tangent, sizeof(float) * 4);
                sm.vertices.push_back(v);
            }
        }

        if (!sm.vertices.empty() && !sm.indices.empty())
            mesh.submeshes.push_back(std::move(sm));
    }

    return mesh;
}

} // namespace bud::asset_pipeline
