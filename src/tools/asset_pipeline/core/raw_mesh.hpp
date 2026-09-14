#pragma once

#include "src/core/bud.raw_mesh.hpp"

namespace bud::asset_pipeline {

using RawVertex = bud::asset::RawVertex;
using RawMaterial = bud::asset::RawMaterial;
using RawSubmesh = bud::asset::RawSubmesh;
using RawMesh = bud::asset::RawMesh;

constexpr uint64_t BUD_RAWMESH_MAGIC = bud::asset::BUD_RAWMESH_MAGIC;
constexpr uint32_t BUD_RAWMESH_VERSION = bud::asset::BUD_RAWMESH_VERSION;

} // namespace bud::asset_pipeline
