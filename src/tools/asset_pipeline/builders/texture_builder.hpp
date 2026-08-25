#pragma once

#include "../core/raw_texture.hpp"
#include <string>

namespace bud::asset_pipeline {

struct TextureBuildOptions {
    bool generate_mips = true;
    bool split_bulk = false;
    bool use_cache = true;
    std::string cache_root = "AssetCache";
};

class TextureBuilder {
public:
    static bool build(const RawTexture& raw, const std::string& output_path, const TextureBuildOptions& options = {});
    static bool build_from_file(const std::string& input_path, const std::string& output_path, const TextureBuildOptions& options = {});
};

} // namespace bud::asset_pipeline
