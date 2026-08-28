#pragma once

#include "../core/raw_mesh.hpp"
#include <vector>
#include <string>

namespace bud::asset_pipeline {

struct MaterialBuildResult {
    std::vector<bud::asset::MaterialDescriptor> descriptors;
    std::vector<std::string> texture_paths;
    std::vector<uint8_t> serialized_chunk;
};

class MaterialBuilder {
public:
    static MaterialBuildResult build(const std::vector<RawMaterial>& materials, const std::vector<std::string>& textures);
};

} // namespace bud::asset_pipeline
