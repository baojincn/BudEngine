#pragma once

#include <vector>
#include <string_view>
#include "src/tools/asset_pipeline/core/raw_mesh.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/physics/bud.cloth.types.hpp"

namespace bud::asset_pipeline {

class ClothBaker {
public:
    static bool is_cloth_material(std::string_view name);
    static std::vector<uint8_t> build(const RawMesh& raw_mesh);
};

} // namespace bud::asset_pipeline
