#pragma once

#include "src/core/bud.asset.types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace bud::asset_pipeline {

constexpr uint64_t BUD_RAWTEXTURE_MAGIC = 0x5458455457415242ULL; // "BRAWTEXT"
constexpr uint32_t BUD_RAWTEXTURE_VERSION = 1;

struct RawTexture {
    std::string source_path;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    bool is_srgb = true;
    std::vector<uint8_t> pixels; // RGBA8

    bool save_binary(const std::string& path) const;
    static std::optional<RawTexture> load_binary(const std::string& path);
};

} // namespace bud::asset_pipeline
