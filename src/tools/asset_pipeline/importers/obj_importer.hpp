#pragma once

#include "../core/raw_mesh.hpp"
#include "../core/raw_scene.hpp"
#include <string>
#include <optional>

namespace bud::asset_pipeline {

class ObjImporter {
public:
    static std::optional<RawMesh> import_from_file(const std::string& filepath, float scale = 1.0f);
    static std::optional<RawScene> import_scene_from_file(const std::string& filepath, float scale = 1.0f);
};

} // namespace bud::asset_pipeline
