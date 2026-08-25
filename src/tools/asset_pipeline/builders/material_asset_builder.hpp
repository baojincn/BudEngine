#pragma once

#include "../core/raw_mesh.hpp"
#include <string>
#include <unordered_map>

namespace bud::asset_pipeline {

class MaterialAssetBuilder {
public:
    static bool build(
        const RawMaterial& raw,
        const std::unordered_map<std::string, uint64_t>& texture_ids,
        const std::string& output_path
    );
};

} // namespace bud::asset_pipeline
