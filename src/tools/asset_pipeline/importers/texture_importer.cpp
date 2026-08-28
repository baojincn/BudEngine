#include "texture_importer.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <iostream>
#include <filesystem>

namespace bud::asset_pipeline {

std::optional<RawTexture> TextureImporter::import_from_file(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "[BudAssetPipeline] Texture file not found: " << path << std::endl;
        return std::nullopt;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* data = stbi_load(path.c_str(), &w, &h, &comp, STBI_rgb_alpha);
    if (!data || w <= 0 || h <= 0) {
        std::cerr << "[BudAssetPipeline] Failed to load image: " << path << std::endl;
        if (data)
            stbi_image_free(data);
        return std::nullopt;
    }

    RawTexture tex;
    tex.source_path = path;
    tex.width = static_cast<uint32_t>(w);
    tex.height = static_cast<uint32_t>(h);
    tex.channels = 4;
    tex.is_srgb = true;

    size_t byte_count = static_cast<size_t>(w) * h * 4;
    tex.pixels.resize(byte_count);
    std::memcpy(tex.pixels.data(), data, byte_count);

    stbi_image_free(data);
    return tex;
}

} // namespace bud::asset_pipeline
