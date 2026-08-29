#pragma once

#include "../core/raw_texture.hpp"
#include <string>
#include <optional>

namespace bud::asset_pipeline {

enum class TextureSemantic {
    Unknown,
    Albedo,
    Normal,
    Roughness,
    Metallic,
    MetallicRoughness,
    SpecularGlossiness,
    Emissive,
    Occlusion,
    AlphaMask
};

struct TextureAlphaInfo {
    bud::asset::AlphaMode alpha_mode = bud::asset::AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    bool has_alpha = false;
};

struct PBRCompanionTextures {
    std::string normal_path;
    std::string roughness_path;
    std::string metallic_path;
    std::string emissive_path;
};

class TextureImporter {
public:
    static std::optional<RawTexture> import_from_file(const std::string& path);
    static TextureAlphaInfo analyze_alpha(const RawTexture& tex);
    static TextureAlphaInfo analyze_alpha(const std::string& path);
    static TextureSemantic detect_semantic_from_filename(const std::string& filename);
    static TextureSemantic detect_semantic_from_pixels(const RawTexture& tex);
    static PBRCompanionTextures find_companion_pbr_textures(const std::string& base_color_path);
    static std::string convert_spec_gloss_to_metallic_roughness(const std::string& spec_gloss_path, const std::string& output_dir = "Cache/textures");
};

} // namespace bud::asset_pipeline
