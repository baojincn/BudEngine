#pragma once

#include "src/core/bud.raw_mesh.hpp"
#include "src/robots/bud.robot.types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace bud::asset_pipeline {

class StlImporter {
public:
    static std::optional<bud::asset::RawMesh> import_from_file(const std::string& filepath, float scale = 1.0f);
    static std::optional<bud::robot::ConvexHullData> extract_collision_geometry(const std::string& filepath, float scale = 1.0f, int max_vertices = 128);
};

} // namespace bud::asset_pipeline
