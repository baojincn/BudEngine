#pragma once

#include <string>

namespace bud::tool {

class AssetProcessor {
public:
    static bool validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path = "", unsigned int max_workers = 0);
};

} // namespace bud::tool
