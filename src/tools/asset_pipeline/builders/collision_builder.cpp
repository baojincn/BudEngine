#include "collision_builder.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <iostream>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>

namespace bud::asset_pipeline {

static void ensure_jolt_initialized() {
    static bool initialized = false;
    if (!initialized) {
        JPH::RegisterDefaultAllocator();
        if (!JPH::Factory::sInstance) {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
        initialized = true;
    }
}

CollisionBuildResult CollisionBuilder::build_collision_lod(
    const std::vector<bud::math::vec3>& in_vertices,
    const std::vector<uint32_t>& in_indices,
    const CollisionBuildOptions& options) {

    CollisionBuildResult result;
    if (in_vertices.empty() || in_indices.empty())
        return result;

    result.shape_type = bud::asset::CollisionShapeType::Mesh;
    result.friction = options.friction;
    result.restitution = options.restitution;

    // If triangle count is already very small (e.g. <= 12 triangles / 36 indices), keep as-is
    if (in_indices.size() <= 36) {
        result.vertices = in_vertices;
        result.indices = in_indices;
        bud::math::vec3 aabb_min(1e30f), aabb_max(-1e30f);
        for (const auto& v : result.vertices) {
            aabb_min = glm::min(aabb_min, v);
            aabb_max = glm::max(aabb_max, v);
        }
        result.aabb_min = aabb_min;
        result.aabb_max = aabb_max;
        return result;
    }

    const float ratio = std::clamp(options.simplification_ratio, 0.01f, 1.0f);
    size_t target_index_count = static_cast<size_t>(in_indices.size() * ratio);
    if (target_index_count < 12)
        target_index_count = 12;

    std::vector<unsigned int> simplified(in_indices.size());
    float result_error = 0.0f;
    const unsigned int flags = meshopt_SimplifyLockBorder;

    size_t simplified_count = meshopt_simplify(
        simplified.data(),
        in_indices.data(),
        in_indices.size(),
        reinterpret_cast<const float*>(in_vertices.data()),
        in_vertices.size(),
        sizeof(bud::math::vec3),
        target_index_count,
        options.target_error,
        flags,
        &result_error
    );

    const unsigned int* final_indices = simplified.data();
    size_t final_index_count = simplified_count;

    // If simplification failed or completely collapsed, fallback to original indices
    if (simplified_count < 3) {
        final_indices = in_indices.data();
        final_index_count = in_indices.size();
    }

    // Compact vertices so that only referenced vertices are stored in CollisionLOD
    std::vector<int32_t> remap(in_vertices.size(), -1);
    std::vector<bud::math::vec3> compacted_vertices;
    std::vector<uint32_t> compacted_indices;
    compacted_indices.reserve(final_index_count);
    bud::math::vec3 aabb_min(1e30f), aabb_max(-1e30f);

    for (size_t i = 0; i < final_index_count; ++i) {
        uint32_t idx = final_indices[i];
        if (idx >= in_vertices.size())
            continue;

        if (remap[idx] < 0) {
            remap[idx] = static_cast<int32_t>(compacted_vertices.size());
            const auto& v = in_vertices[idx];
            compacted_vertices.push_back(v);
            aabb_min = glm::min(aabb_min, v);
            aabb_max = glm::max(aabb_max, v);
        }
        compacted_indices.push_back(static_cast<uint32_t>(remap[idx]));
    }

    // Optimize vertex cache for collision raycasts/narrowphase
    if (!compacted_indices.empty()) {
        meshopt_optimizeVertexCache(
            compacted_indices.data(),
            compacted_indices.data(),
            compacted_indices.size(),
            compacted_vertices.size()
        );
    }

    result.vertices = std::move(compacted_vertices);
    result.indices = std::move(compacted_indices);
    result.aabb_min = aabb_min;
    result.aabb_max = aabb_max;
    return result;
}

CollisionBuildResult CollisionBuilder::build_convex_hull(
    const std::vector<bud::math::vec3>& in_vertices,
    const CollisionBuildOptions& options) {

    CollisionBuildResult result;
    if (in_vertices.empty())
        return result;

    ensure_jolt_initialized();

    result.shape_type = bud::asset::CollisionShapeType::ConvexHull;
    result.friction = options.friction;
    result.restitution = options.restitution;

    JPH::Array<JPH::Vec3> in_positions;
    in_positions.reserve(in_vertices.size());
    bud::math::vec3 aabb_min(1e30f), aabb_max(-1e30f);

    for (const auto& v : in_vertices) {
        in_positions.push_back(JPH::Vec3(v.x, v.y, v.z));
        aabb_min = glm::min(aabb_min, v);
        aabb_max = glm::max(aabb_max, v);
    }
    result.aabb_min = aabb_min;
    result.aabb_max = aabb_max;

    const char* error_msg = nullptr;
    constexpr float hull_tolerance = 1.0e-3f;
    const int max_verts = static_cast<int>(std::clamp(options.max_convex_vertices, 8u, 256u));

    JPH::ConvexHullBuilder builder(in_positions);
    JPH::ConvexHullBuilder::EResult hull_res = builder.Initialize(max_verts, hull_tolerance, error_msg);

    if (hull_res == JPH::ConvexHullBuilder::EResult::Success ||
        hull_res == JPH::ConvexHullBuilder::EResult::MaxVerticesReached) {

        std::unordered_map<int, uint32_t> global_to_local;
        const auto& faces = builder.GetFaces();

        for (const auto* face : faces) {
            if (!face || face->mRemoved)
                continue;

            std::vector<int> face_indices;
            const JPH::ConvexHullBuilder::Edge* first_edge = face->mFirstEdge;
            const JPH::ConvexHullBuilder::Edge* edge = first_edge;

            while (edge) {
                face_indices.push_back(edge->mStartIdx);
                edge = edge->mNextEdge;
                if (edge == first_edge)
                    break;
            }

            if (face_indices.size() < 3)
                continue;

            std::vector<uint32_t> local_poly;
            local_poly.reserve(face_indices.size());
            for (int orig_idx : face_indices) {
                auto it = global_to_local.find(orig_idx);
                if (it == global_to_local.end()) {
                    uint32_t new_idx = static_cast<uint32_t>(result.vertices.size());
                    const JPH::Vec3& p = in_positions[orig_idx];
                    result.vertices.push_back(bud::math::vec3(p.GetX(), p.GetY(), p.GetZ()));
                    global_to_local[orig_idx] = new_idx;
                    local_poly.push_back(new_idx);
                } else {
                    local_poly.push_back(it->second);
                }
            }

            for (size_t i = 1; i + 1 < local_poly.size(); ++i) {
                result.indices.push_back(local_poly[0]);
                result.indices.push_back(local_poly[i]);
                result.indices.push_back(local_poly[i + 1]);
            }
        }
    } else {
        // Fallback if ConvexHullBuilder fails
        result.vertices = in_vertices;
        for (uint32_t i = 0; i < static_cast<uint32_t>(in_vertices.size()); ++i)
            result.indices.push_back(i);
    }

    return result;
}

std::vector<uint8_t> CollisionBuilder::serialize_chunk(const CollisionBuildResult& result) {
    std::vector<uint8_t> buffer;
    if (!result.is_valid())
        return buffer;

    bud::asset::CollisionChunkHeader header{};
    header.magic = bud::asset::COLLISION_CHUNK_MAGIC;
    header.version = bud::asset::COLLISION_CHUNK_VERSION;
    header.shape_type = static_cast<uint32_t>(result.shape_type);
    header.vertex_count = static_cast<uint32_t>(result.vertices.size());
    header.index_count = static_cast<uint32_t>(result.indices.size());
    header.aabb_min[0] = result.aabb_min.x;
    header.aabb_min[1] = result.aabb_min.y;
    header.aabb_min[2] = result.aabb_min.z;
    header.aabb_max[0] = result.aabb_max.x;
    header.aabb_max[1] = result.aabb_max.y;
    header.aabb_max[2] = result.aabb_max.z;
    header.friction = result.friction;
    header.restitution = result.restitution;

    const size_t vert_bytes = result.vertices.size() * sizeof(bud::math::vec3);
    const size_t idx_bytes = result.indices.size() * sizeof(uint32_t);
    const size_t total_size = sizeof(header) + vert_bytes + idx_bytes;

    buffer.resize(total_size);
    uint8_t* dst = buffer.data();

    std::memcpy(dst, &header, sizeof(header));
    dst += sizeof(header);

    if (vert_bytes > 0) {
        std::memcpy(dst, result.vertices.data(), vert_bytes);
        dst += vert_bytes;
    }

    if (idx_bytes > 0) {
        std::memcpy(dst, result.indices.data(), idx_bytes);
    }

    return buffer;
}

std::optional<CollisionBuildResult> CollisionBuilder::deserialize_chunk(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(bud::asset::CollisionChunkHeader))
        return std::nullopt;

    bud::asset::CollisionChunkHeader header{};
    std::memcpy(&header, data, sizeof(header));

    if (header.magic != bud::asset::COLLISION_CHUNK_MAGIC || header.version != bud::asset::COLLISION_CHUNK_VERSION)
        return std::nullopt;

    const size_t vert_bytes = header.vertex_count * sizeof(bud::math::vec3);
    const size_t idx_bytes = header.index_count * sizeof(uint32_t);
    if (sizeof(header) + vert_bytes + idx_bytes > size)
        return std::nullopt;

    CollisionBuildResult result;
    result.shape_type = static_cast<bud::asset::CollisionShapeType>(header.shape_type);
    result.friction = header.friction;
    result.restitution = header.restitution;
    result.aabb_min = bud::math::vec3(header.aabb_min[0], header.aabb_min[1], header.aabb_min[2]);
    result.aabb_max = bud::math::vec3(header.aabb_max[0], header.aabb_max[1], header.aabb_max[2]);

    result.vertices.resize(header.vertex_count);
    const uint8_t* src = data + sizeof(header);
    if (vert_bytes > 0) {
        std::memcpy(result.vertices.data(), src, vert_bytes);
        src += vert_bytes;
    }

    result.indices.resize(header.index_count);
    if (idx_bytes > 0) {
        std::memcpy(result.indices.data(), src, idx_bytes);
    }

    return result;
}

} // namespace bud::asset_pipeline
