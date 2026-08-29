#include "material_asset_builder.hpp"
#include "../core/serializer.hpp"
#include "../core/support.hpp"
#include <iostream>
#include <filesystem>
#include <cstring>

namespace bud::asset_pipeline {

bool MaterialAssetBuilder::build(
    const RawMaterial& raw,
    const std::unordered_map<std::string, uint64_t>& texture_ids,
    const std::string& output_path) {

    bud::asset::RuntimeMaterialHeader header{};
    header.magic = bud::asset::MATERIAL_MAGIC;
    header.version = bud::asset::MATERIAL_VERSION;
    header.alpha_mode = static_cast<uint8_t>(raw.alpha_mode);
    header.double_sided = raw.double_sided ? 1 : 0;
    header.shading_model = 0; // Standard PBR Default Lit
    header.base_color_factor[0] = raw.base_color_factor[0];
    header.base_color_factor[1] = raw.base_color_factor[1];
    header.base_color_factor[2] = raw.base_color_factor[2];
    header.base_color_factor[3] = raw.base_color_factor[3];
    header.metallic_factor = raw.metallic_factor;
    header.roughness_factor = raw.roughness_factor;
    header.alpha_cutoff = raw.alpha_cutoff;

    // Slot 0: Base Color Texture
    // Slot 1: Normal Texture
    // Slot 2: Metallic / Roughness Texture
    // Slot 3: Emissive Texture
    header.texture_count = 0;
    for (int s = 0; s < 8; ++s) header.texture_asset_ids[s] = 0;

    auto bind_slot = [&](size_t slot, const std::string& path) {
        if (!path.empty()) {
            auto it = texture_ids.find(path);
            if (it != texture_ids.end()) {
                header.texture_asset_ids[slot] = it->second;
                if (slot + 1 > header.texture_count) header.texture_count = static_cast<uint32_t>(slot + 1);
            }
        }
    };

    bind_slot(0, raw.base_color_texture_path);
    bind_slot(1, raw.normal_texture_path);
    bind_slot(2, raw.metallic_roughness_texture_path);
    bind_slot(3, raw.emissive_texture_path);

    std::vector<uint8_t> payload(sizeof(header));
    std::memcpy(payload.data(), &header, sizeof(header));

    std::hash<std::string> hasher;
    uint64_t asset_id = hasher(raw.name.empty() ? output_path : raw.name);

    BudAssetWriter writer(AssetType::Material, asset_id);
    writer.add_chunk(AssetChunkType::Material, payload.data(), payload.size());

    std::error_code ec;
    std::filesystem::path out_p(output_path);
    if (out_p.has_parent_path())
        std::filesystem::create_directories(out_p.parent_path(), ec);

    if (!writer.save_to_file(output_path)) {
        std::cerr << "[BudAssetPipeline] Failed to write Material .budasset: " << output_path << std::endl;
        return false;
    }

    return true;
}

} // namespace bud::asset_pipeline
