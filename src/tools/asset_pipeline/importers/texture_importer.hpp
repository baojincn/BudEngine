#pragma once

#include "../core/raw_texture.hpp"
#include <string>
#include <optional>

namespace bud::asset_pipeline {

class TextureImporter {
public:
    static std::optional<RawTexture> import_from_file(const std::string& path);
};

} // namespace bud::asset_pipeline
