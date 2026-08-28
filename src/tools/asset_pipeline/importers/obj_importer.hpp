#pragma once

#include "../core/raw_mesh.hpp"
#include <string>
#include <optional>

namespace bud::asset_pipeline {

class ObjImporter {
public:
    static std::optional<RawMesh> import_from_file(const std::string& filepath);
};

} // namespace bud::asset_pipeline
