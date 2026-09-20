#pragma once

#include "src/robots/bud.robot.types.hpp"
#include <string>
#include <optional>

namespace bud::asset_pipeline {

struct UrdfParseOptions {
    std::string package_root = "";   // Base directory for resolving package:// URIs
    bool auto_detect_root = true;    // Automatically detect root link with no parent
};

class UrdfParser {
public:
    static std::optional<bud::robot::RobotDef> parse_file(const std::string& filepath, const UrdfParseOptions& options = {});
    static std::optional<bud::robot::RobotDef> parse_string(const std::string& xml_content, const std::string& base_dir = "", const UrdfParseOptions& options = {});

    // Resolves package:// and relative mesh URIs into absolute file system paths
    static std::string resolve_mesh_path(const std::string& raw_path, const std::string& urdf_dir, const std::string& package_root);
};

} // namespace bud::asset_pipeline
