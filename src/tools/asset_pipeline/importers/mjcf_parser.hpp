#pragma once

#include "src/robots/bud.robot.types.hpp"
#include <string>
#include <optional>
#include <unordered_map>

namespace bud::asset_pipeline {

struct MjcfParseOptions {
    std::string mesh_dir_override = "";
    float default_scale = 1.0f;
};

class MjcfParser {
public:
    static std::optional<bud::robot::RobotDef> parse_file(
        const std::string& filepath,
        const MjcfParseOptions& options = {}
    );

    static std::optional<bud::robot::RobotDef> parse_string(
        const std::string& xml_content,
        const std::string& base_dir = "",
        const MjcfParseOptions& options = {}
    );
};

} // namespace bud::asset_pipeline
