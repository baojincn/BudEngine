#include "material_builder.hpp"
#include <cstring>
#include <unordered_map>

namespace bud::asset_pipeline {

MaterialBuildResult MaterialBuilder::build(const std::vector<RawMaterial>& materials, const std::vector<std::string>& textures) {
    MaterialBuildResult result;
    result.texture_paths = textures;

    std::unordered_map<std::string, uint32_t> tex_map;
    for (size_t i = 0; i < textures.size(); ++i) {
        tex_map[textures[i]] = static_cast<uint32_t>(i);
    }

    auto get_or_add_texture = [&](const std::string& path) -> uint32_t {
        if (path.empty()) return 0xFFFFFFFF;
        auto it = tex_map.find(path);
        if (it != tex_map.end()) {
            return it->second;
        }
        uint32_t new_idx = static_cast<uint32_t>(result.texture_paths.size());
        result.texture_paths.push_back(path);
        tex_map[path] = new_idx;
        return new_idx;
    };

    for (const auto& rm : materials) {
        bud::asset::MaterialDescriptor md{};
        md.base_color_texture = get_or_add_texture(rm.base_color_texture_path);
        if (md.base_color_texture == 0xFFFFFFFF) md.base_color_texture = 0;

        md.normal_texture = get_or_add_texture(rm.normal_texture_path);
        md.metallic_roughness_texture = get_or_add_texture(rm.metallic_roughness_texture_path);
        md.emissive_texture = get_or_add_texture(rm.emissive_texture_path);
        md.metallic_factor = rm.metallic_factor;
        md.roughness_factor = rm.roughness_factor;

        md.alpha_mode = static_cast<uint8_t>(rm.alpha_mode);
        md.double_sided = rm.double_sided ? 1 : 0;
        md.alpha_cutoff = rm.alpha_cutoff;
        result.descriptors.push_back(md);
    }

    // Serialize Material Chunk binary payload:
    // [uint32 material_count][MaterialDescriptor...][uint32 texture_count][Null-terminated strings...]
    uint32_t mat_count = static_cast<uint32_t>(result.descriptors.size());
    uint32_t tex_count = static_cast<uint32_t>(result.texture_paths.size());

    size_t total_size = sizeof(uint32_t) + mat_count * sizeof(bud::asset::MaterialDescriptor) + sizeof(uint32_t);
    for (const auto& t : result.texture_paths)
        total_size += t.length() + 1;

    result.serialized_chunk.resize(total_size);
    uint8_t* dst = result.serialized_chunk.data();

    std::memcpy(dst, &mat_count, sizeof(uint32_t));
    dst += sizeof(uint32_t);

    if (mat_count > 0) {
        std::memcpy(dst, result.descriptors.data(), mat_count * sizeof(bud::asset::MaterialDescriptor));
        dst += mat_count * sizeof(bud::asset::MaterialDescriptor);
    }

    std::memcpy(dst, &tex_count, sizeof(uint32_t));
    dst += sizeof(uint32_t);

    for (const auto& t : result.texture_paths) {
        std::memcpy(dst, t.c_str(), t.length() + 1);
        dst += t.length() + 1;
    }

    return result;
}

} // namespace bud::asset_pipeline
