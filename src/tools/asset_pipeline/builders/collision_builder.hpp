#pragma once

#include <vector>
#include <optional>
#include <cstdint>
#include <string>
#include "src/core/bud.math.hpp"
#include "src/core/bud.asset.types.hpp"

namespace bud::asset_pipeline {

struct CollisionBuildOptions {
    bud::asset::CollisionShapeType preferred_shape = bud::asset::CollisionShapeType::Mesh;
    float simplification_ratio = 0.15f; // Target 15% faces for CollisionLOD
    float target_error = 0.02f;         // 2% relative geometric error threshold
    uint32_t max_convex_vertices = 32;  // Max vertices when building convex hull
    float friction = 0.85f;
    float restitution = 0.05f;
};

struct CollisionBuildResult {
    bud::asset::CollisionShapeType shape_type = bud::asset::CollisionShapeType::Mesh;
    std::vector<bud::math::vec3> vertices;
    std::vector<uint32_t> indices;
    bud::math::vec3 aabb_min{ 0.0f };
    bud::math::vec3 aabb_max{ 0.0f };
    float friction = 0.85f;
    float restitution = 0.05f;

    bool is_valid() const {
        return !vertices.empty() && !indices.empty();
    }
};

class CollisionBuilder {
public:
    // Builds CollisionLOD using pure position topological decimation with border locking
    static CollisionBuildResult build_collision_lod(
        const std::vector<bud::math::vec3>& in_vertices,
        const std::vector<uint32_t>& in_indices,
        const CollisionBuildOptions& options = {}
    );

    // Builds a watertight Convex Hull using Jolt ConvexHullBuilder
    static CollisionBuildResult build_convex_hull(
        const std::vector<bud::math::vec3>& in_vertices,
        const CollisionBuildOptions& options = {}
    );

    // Serializes a CollisionBuildResult into a binary buffer matching CollisionChunkHeader
    static std::vector<uint8_t> serialize_chunk(const CollisionBuildResult& result);

    // Deserializes a CollisionChunkHeader buffer into a CollisionBuildResult
    static std::optional<CollisionBuildResult> deserialize_chunk(const uint8_t* data, size_t size);
};

} // namespace bud::asset_pipeline
