#pragma once

#include "src/core/bud.asset.types.hpp"
#include <string>
#include <vector>
#include <memory>

namespace bud::asset_pipeline {

using AssetType = bud::asset::AssetType;
using AssetChunkType = bud::asset::AssetChunkType;
using AssetChunkFlags = bud::asset::AssetChunkFlags;
using BudAssetHeader = bud::asset::BudAssetHeader;
using AssetChunkEntry = bud::asset::AssetChunkEntry;

using VGCluster = bud::asset::VGCluster;
using VGClusterGroup = bud::asset::VGClusterGroup;
using VGPageStreamingState = bud::asset::VGPageStreamingState;
using VGPageDependency = bud::asset::VGPageDependency;
using VGHeader = bud::asset::VGHeader;
using VGPageDataHeader = bud::asset::VGPageDataHeader;
using VGPackedVertex = bud::asset::VGPackedVertex;

struct AssetChunkPayload {
    AssetChunkType type;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
};

// Generates a 64-bit deterministic hash from string path/name
uint64_t compute_asset_id(const std::string& path);

} // namespace bud::asset_pipeline
