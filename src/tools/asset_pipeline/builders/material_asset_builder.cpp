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
    header.base_color_factor[0] = 1.0f;
    header.base_color_factor[1] = 1.0f;
    header.base_color_factor[2] = 1.0f;
    header.base_color_factor[3] = 1.0f;
    header.metallic_factor = 0.0f;
    header.roughness_factor = 0.5f;
    header.alpha_cutoff = raw.alpha_cutoff;

    // Slot 0: Base Color Texture
    if (!raw.base_color_texture_path.empty()) {
        auto it = texture_ids.find(raw.base_color_texture_path);
        if (it != texture_ids.end()) {
            header.texture_count = 1;
            header.texture_asset_ids[0] = it->second;
        }
    }

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
