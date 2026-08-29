#pragma once

#include "src/core/bud.asset.types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace bud::asset_pipeline {

constexpr uint64_t BUD_RAWMESH_MAGIC = 0x53454D57415242ULL; // "BRAWMES"
constexpr uint32_t BUD_RAWMESH_VERSION = 1;

struct RawVertex {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 1.0f, 0.0f };
    float uv[2] = { 0.0f, 0.0f };
    float tangent[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
};

struct RawMaterial {
    std::string name;
    std::string base_color_texture_path;
    std::string normal_texture_path;
    std::string metallic_roughness_texture_path;
    std::string emissive_texture_path;
    float metallic_factor = 0.0f;
    float roughness_factor = 0.5f;
    float base_color_factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissive_factor[3] = { 0.0f, 0.0f, 0.0f };
    bud::asset::AlphaMode alpha_mode = bud::asset::AlphaMode::Opaque;
    bool double_sided = false;
    float alpha_cutoff = 0.5f;
};

struct RawSubmesh {
    std::string name;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t material_index = 0;
};

struct RawMesh {
    std::string source_path;
    std::vector<RawVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RawSubmesh> submeshes;
    std::vector<RawMaterial> materials;
    std::vector<std::string> textures;

    float aabb_min[3] = { 0.0f, 0.0f, 0.0f };
    float aabb_max[3] = { 0.0f, 0.0f, 0.0f };

    void compute_bounds();
    std::vector<uint8_t> serialize_binary() const;
    static std::optional<RawMesh> deserialize_binary(const uint8_t* data, size_t size);

    bool save_binary(const std::string& path) const;
    static std::optional<RawMesh> load_binary(const std::string& path);

    bool save_json(const std::string& path) const;
    static std::optional<RawMesh> load_json(const std::string& path);

    static std::optional<RawMesh> load_from_file(const std::string& path);
};

} // namespace bud::asset_pipeline
