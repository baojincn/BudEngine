#pragma once

#include "../core/internal_mesh.hpp"
#include "../core/asset_format.hpp"
#include <vector>
#include <string>

namespace bud::asset_pipeline {

struct VGBuildResult {
    VGHeader header{};
    std::vector<VGCluster> clusters;
    std::vector<VGClusterGroup> groups;
    std::vector<VGPageStreamingState> pages;
    std::vector<VGPageDependency> dependencies;
    std::vector<std::vector<uint8_t>> raw_page_data;
};

class VirtualGeometryBuilder {
public:
    static VGBuildResult build(const InternalMesh& mesh);
    static bool dump_json(const VGBuildResult& result, const InternalMesh& mesh, const std::string& path);
};

} // namespace bud::asset_pipeline
