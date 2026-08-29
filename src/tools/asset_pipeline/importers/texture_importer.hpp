#pragma once

#include "../core/raw_texture.hpp"
#include <string>
#include <optional>

namespace bud::asset_pipeline {

struct TextureAlphaInfo {
    bud::asset::AlphaMode alpha_mode = bud::asset::AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    bool has_alpha = false;
};

class TextureImporter {
public:
    static std::optional<RawTexture> import_from_file(const std::string& path);
    static TextureAlphaInfo analyze_alpha(const RawTexture& tex);
    static TextureAlphaInfo analyze_alpha(const std::string& path);
};

} // namespace bud::asset_pipeline
