#include "virtual_geometry_builder.hpp"
#include "src/core/bud.core.hpp"
#include <meshoptimizer.h>
#include <metis.h>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

namespace bud::asset_pipeline {

using namespace bud::literals;

namespace {

struct InternalCluster {
    uint32_t material_index = 0;
    uint32_t level = 0;
    uint32_t group_index = bud::asset::INVALID_INDEX;
    uint32_t source_group = bud::asset::INVALID_INDEX;
    std::vector<bud::asset::Vertex> vertices;
    std::vector<uint32_t> indices;

    float bounds_min[3] = { 0.0f, 0.0f, 0.0f };
    float bounds_max[3] = { 0.0f, 0.0f, 0.0f };
    float lod_bounds_center[3] = { 0.0f, 0.0f, 0.0f };
    float lod_bounds_radius = 0.0f;
    float cone_axis[3] = { 0.0f, 1.0f, 0.0f };
    float cone_cutoff = 1.0f;
    float lod_error = 0.0f;
    float parent_lod_error = 0.0f;
};

struct InternalGroup {
    uint32_t level = 0;
    uint32_t cluster_start = 0;
    uint32_t cluster_count = 0;
    uint32_t children_start = 0;
    uint32_t children_count = 0;
    float lod_bounds_center[3] = { 0.0f, 0.0f, 0.0f };
    float lod_bounds_radius = 0.0f;
    float lod_error = 0.0f;
};

struct InternalPage {
    std::vector<uint32_t> clusters;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t size_in_bytes = 0;
    uint32_t dependency_page_id = bud::asset::INVALID_INDEX;
    uint32_t lod_level = 0;
    bool is_root = false;
};

// GPU-side cluster descriptor (7 dwords) + cull data (5 dwords) embedded in each page
constexpr uint32_t kClusterDescDwords = 7u;
constexpr uint32_t kClusterCullDwords = 5u;
constexpr uint32_t kClusterGPUOverhead = (kClusterDescDwords + kClusterCullDwords) * sizeof(uint32_t);

void compute_cluster_bounds(InternalCluster& cb) {
    if (cb.vertices.empty())
        return;

    cb.bounds_min[0] = cb.bounds_min[1] = cb.bounds_min[2] = FLT_MAX;
    cb.bounds_max[0] = cb.bounds_max[1] = cb.bounds_max[2] = -FLT_MAX;

    for (const auto& v : cb.vertices) {
        for (int k = 0; k < 3; ++k) {
            cb.bounds_min[k] = std::min(cb.bounds_min[k], v.position[k]);
            cb.bounds_max[k] = std::max(cb.bounds_max[k], v.position[k]);
        }
    }

    meshopt_Bounds mb = meshopt_computeClusterBounds(
        cb.indices.data(),
        cb.indices.size(),
        reinterpret_cast<const float*>(cb.vertices.data()),
        cb.vertices.size(),
        sizeof(bud::asset::Vertex));

    cb.lod_bounds_center[0] = mb.center[0];
    cb.lod_bounds_center[1] = mb.center[1];
    cb.lod_bounds_center[2] = mb.center[2];
    cb.lod_bounds_radius = mb.radius;

    if (mb.cone_cutoff_s8 == 127) {
        meshopt_Bounds vert_mb = meshopt_computeMeshletBounds(
            nullptr, nullptr, 0,
            reinterpret_cast<const float*>(cb.vertices.data()),
            cb.vertices.size(),
            sizeof(bud::asset::Vertex));
        cb.cone_axis[0] = vert_mb.cone_axis_s8[0] / 127.0f;
        cb.cone_axis[1] = vert_mb.cone_axis_s8[1] / 127.0f;
        cb.cone_axis[2] = vert_mb.cone_axis_s8[2] / 127.0f;
        cb.cone_cutoff = vert_mb.cone_cutoff_s8 / 127.0f;
    } else {
        cb.cone_axis[0] = mb.cone_axis_s8[0] / 127.0f;
        cb.cone_axis[1] = mb.cone_axis_s8[1] / 127.0f;
        cb.cone_axis[2] = mb.cone_axis_s8[2] / 127.0f;
        cb.cone_cutoff = mb.cone_cutoff_s8 / 127.0f;
    }
}

uint32_t compute_morton_code_3d(const float* position, const float bounding_origin[3], const float bounding_scale[3]) {
    auto part_1_by_2 = [](uint32_t value) -> uint32_t {
        value &= 0x3ffu;
        value = (value | (value << 16)) & 0x030000FFu;
        value = (value | (value << 8)) & 0x0300F00Fu;
        value = (value | (value << 4)) & 0x030C30C3u;
        value = (value | (value << 2)) & 0x09249249u;
        return value;
    };
    auto quantize_axis = [&](float value, int axis) -> uint32_t {
        float normalized_value = std::clamp((value - bounding_origin[axis]) * bounding_scale[axis], 0.0f, 1023.0f);
        return static_cast<uint32_t>(normalized_value);
    };
    return (part_1_by_2(quantize_axis(position[0], 0)) |
        (part_1_by_2(quantize_axis(position[1], 1)) << 1) |
        (part_1_by_2(quantize_axis(position[2], 2)) << 2));
}

void generate_leaf_clusters(const std::vector<bud::asset::Vertex>& vertices,
                            const std::vector<uint32_t>& indices,
                            uint32_t material_index,
                            std::vector<InternalCluster>& out) {
    const uint32_t max_vertices = bud::asset::VG_MAX_CLUSTER_VERTICES;
    const uint32_t max_triangles = bud::asset::VG_MAX_CLUSTER_TRIANGLES;

    if (vertices.empty() || indices.empty())
        return;

    std::vector<unsigned int> cached_indices(indices.size());
    meshopt_optimizeVertexCache(cached_indices.data(), indices.data(), indices.size(), vertices.size());

    size_t max_meshlets = meshopt_buildMeshletsBound(cached_indices.size(), max_vertices, max_triangles);
    std::vector<meshopt_Meshlet> meshlets(max_meshlets);
    std::vector<unsigned int> mv(max_meshlets * max_vertices);
    std::vector<unsigned char> mt(max_meshlets * max_triangles * 3);

    size_t count = meshopt_buildMeshletsScan(meshlets.data(), mv.data(), mt.data(),
                                             cached_indices.data(), cached_indices.size(),
                                             vertices.size(),
                                             max_vertices, max_triangles);

    for (size_t i = 0; i < count; ++i) {
        auto& m = meshlets[i];
        meshopt_optimizeMeshlet(&mv[m.vertex_offset], &mt[m.triangle_offset], m.triangle_count, m.vertex_count);

        InternalCluster cluster;
        cluster.material_index = material_index;
        cluster.level = 0;
        cluster.vertices.reserve(m.vertex_count);
        cluster.indices.reserve(m.triangle_count * 3);

        for (uint32_t v = 0; v < m.vertex_count; ++v)
            cluster.vertices.push_back(vertices[mv[m.vertex_offset + v]]);
        for (uint32_t t = 0; t < m.triangle_count * 3; ++t)
            cluster.indices.push_back(mt[m.triangle_offset + t]);

        compute_cluster_bounds(cluster);
        out.push_back(std::move(cluster));
    }
}

std::vector<InternalCluster> simplify_cluster_set(
    const std::vector<uint32_t>& cluster_indices,
    const std::vector<InternalCluster>& clusters,
    uint32_t level,
    uint32_t material_index,
    float group_radius,
    const std::unordered_set<uint64_t>* global_shared_edges = nullptr) {

    std::vector<bud::asset::Vertex> merged_v;
    std::vector<uint32_t> merged_i;
    size_t base = 0;
    float max_child_lod_error = 0.0f;

    for (uint32_t ci : cluster_indices) {
        const auto& c = clusters[ci];
        max_child_lod_error = std::max(max_child_lod_error, c.lod_error);
        for (const auto& v : c.vertices)
            merged_v.push_back(v);
        for (uint32_t idx : c.indices)
            merged_i.push_back(static_cast<uint32_t>(base + idx));
        base += c.vertices.size();
    }

    if (merged_v.empty() || merged_i.empty())
        return {};

    meshopt_Stream streams[4];
    streams[0].data = &merged_v[0].position[0];
    streams[0].size = sizeof(float) * 3;
    streams[0].stride = sizeof(bud::asset::Vertex);
    streams[1].data = &merged_v[0].normal[0];
    streams[1].size = sizeof(float) * 3;
    streams[1].stride = sizeof(bud::asset::Vertex);
    streams[2].data = &merged_v[0].uv[0];
    streams[2].size = sizeof(float) * 2;
    streams[2].stride = sizeof(bud::asset::Vertex);
    streams[3].data = &merged_v[0].tangent[0];
    streams[3].size = sizeof(float) * 4;
    streams[3].stride = sizeof(bud::asset::Vertex);

    std::vector<unsigned int> remap(merged_v.size(), ~0u);
    size_t dedup_count = meshopt_generateVertexRemapMulti(remap.data(), merged_i.data(), merged_i.size(),
                                                          merged_v.size(), streams, 4);
    std::vector<bud::asset::Vertex> deduped_v(dedup_count);
    for (size_t i = 0; i < merged_v.size(); ++i) {
        if (remap[i] != ~0u)
            deduped_v[remap[i]] = merged_v[i];
    }
    std::vector<uint32_t> deduped_i(merged_i.size());
    meshopt_remapIndexBuffer(deduped_i.data(), merged_i.data(), merged_i.size(), remap.data());

    // Lock boundary vertices of the MERGED GROUP (not internal cluster boundaries).
    std::vector<unsigned char> vertex_lock(deduped_v.size(), 0);
    {
        std::unordered_map<uint64_t, uint32_t> group_edge_count;
        auto add_edge = [&](uint32_t a, uint32_t b) {
            const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
            ++group_edge_count[key];
        };
        for (size_t t = 0; t + 2 < deduped_i.size(); t += 3) {
            add_edge(deduped_i[t], deduped_i[t + 1]);
            add_edge(deduped_i[t + 1], deduped_i[t + 2]);
            add_edge(deduped_i[t + 2], deduped_i[t]);
        }
        for (const auto& [edge_key, cnt] : group_edge_count) {
            if (cnt == 1) {
                uint32_t v0 = static_cast<uint32_t>(edge_key >> 32);
                uint32_t v1 = static_cast<uint32_t>(edge_key);
                if (v0 < vertex_lock.size()) vertex_lock[v0] = 1;
                if (v1 < vertex_lock.size()) vertex_lock[v1] = 1;
            }
        }
    }

    if (global_shared_edges && !global_shared_edges->empty()) {
        auto get_hash = [](const bud::asset::Vertex& v) -> uint32_t {
            uint32_t hx = *reinterpret_cast<const uint32_t*>(&v.position[0]);
            uint32_t hy = *reinterpret_cast<const uint32_t*>(&v.position[1]);
            uint32_t hz = *reinterpret_cast<const uint32_t*>(&v.position[2]);
            return hx ^ (hy * 16777619u) ^ (hz * 2166136261u);
        };
        for (size_t t = 0; t + 2 < deduped_i.size(); t += 3) {
            uint32_t i0 = deduped_i[t];
            uint32_t i1 = deduped_i[t + 1];
            uint32_t i2 = deduped_i[t + 2];
            auto check_edge = [&](uint32_t a, uint32_t b) {
                if (a < deduped_v.size() && b < deduped_v.size()) {
                    uint32_t ha = get_hash(deduped_v[a]);
                    uint32_t hb = get_hash(deduped_v[b]);
                    uint64_t key = (static_cast<uint64_t>(std::min(ha, hb)) << 32) | std::max(ha, hb);
                    if (global_shared_edges->count(key)) {
                        vertex_lock[a] = vertex_lock[b] = 1;
                    }
                }
            };
            check_edge(i0, i1);
            check_edge(i1, i2);
            check_edge(i2, i0);
        }
    }

    std::vector<unsigned int> simplified(deduped_i.size());
    std::vector<float> attrs(deduped_v.size() * 5);
    for (size_t i = 0; i < deduped_v.size(); ++i) {
        attrs[i * 5 + 0] = deduped_v[i].uv[0];
        attrs[i * 5 + 1] = deduped_v[i].uv[1];
        attrs[i * 5 + 2] = deduped_v[i].normal[0];
        attrs[i * 5 + 3] = deduped_v[i].normal[1];
        attrs[i * 5 + 4] = deduped_v[i].normal[2];
    }
    const float weights[5] = { 0.02f, 0.02f, 0.05f, 0.05f, 0.05f };
    const float level_scale = 1.0f + 0.8f * static_cast<float>(level);

    // Standard Unit: 1.0f == 1.0 cm.
    // Target 50% triangle reduction per level
    size_t target_indices = std::max<size_t>(bud::asset::VG_MAX_CLUSTER_TRIANGLES * 3 / 2, deduped_i.size() / 2);
    const float base_error = std::max(2.0_mm, group_radius * (0.003f + 0.005f * static_cast<float>(level)));
    float result_error = 0.0f;

    size_t simplified_count = meshopt_simplifyWithAttributes(
        simplified.data(), deduped_i.data(), deduped_i.size(),
        &deduped_v[0].position[0], deduped_v.size(), sizeof(bud::asset::Vertex),
        attrs.data(), sizeof(float) * 5, weights, 5,
        vertex_lock.data(),
        target_indices, base_error, 0, &result_error);

    if (simplified_count == 0 || simplified_count >= deduped_i.size()) {
        // Retry with relaxed lock for higher levels
        simplified_count = meshopt_simplifyWithAttributes(
            simplified.data(), deduped_i.data(), deduped_i.size(),
            &deduped_v[0].position[0], deduped_v.size(), sizeof(bud::asset::Vertex),
            attrs.data(), sizeof(float) * 5, weights, 5,
            nullptr,
            target_indices, base_error * 2.0f, 0, &result_error);
    }

    if (simplified_count == 0) {
        simplified_count = deduped_i.size();
        std::copy(deduped_i.begin(), deduped_i.end(), simplified.begin());
        result_error = 0.0f;
    }

    // Convert meshopt normalized error to object-space error (cm) and scale with level
    // Level 1: ~0.4 - 0.6 cm (LOD0 -> LOD1 transition around 3-4 meters)
    // Level 2: ~1.0 - 1.4 cm (LOD1 -> LOD2 transition around 7-10 meters)
    // Level 3: ~2.0 - 2.8 cm (LOD2 -> LOD3 transition around 15-20 meters)
    float level_error_step = std::max(2.0_mm, std::min(group_radius * 0.004f, 0.5_cm)) * static_cast<float>(level);
    float max_level_error = 0.6_cm * static_cast<float>(level);
    float added_error = std::min(std::max(result_error * group_radius, level_error_step), max_level_error);
    float final_lod_error = max_child_lod_error + added_error;

    const uint32_t max_indices = bud::asset::VG_MAX_CLUSTER_TRIANGLES * 3;
    if (simplified.size() <= max_indices && deduped_v.size() <= bud::asset::VG_MAX_CLUSTER_VERTICES) {
        InternalCluster cluster;
        cluster.material_index = material_index;
        cluster.level = level;
        cluster.vertices = std::move(deduped_v);
        cluster.indices = std::move(simplified);
        cluster.lod_error = final_lod_error;
        compute_cluster_bounds(cluster);
        return { std::move(cluster) };
    }

    std::vector<unsigned int> cached(simplified.size());
    meshopt_optimizeVertexCache(cached.data(), simplified.data(), simplified.size(), deduped_v.size());

    size_t max_m = meshopt_buildMeshletsBound(cached.size(), bud::asset::VG_MAX_CLUSTER_VERTICES, bud::asset::VG_MAX_CLUSTER_TRIANGLES);
    std::vector<meshopt_Meshlet> meshlets(max_m);
    std::vector<unsigned int> mv(max_m * bud::asset::VG_MAX_CLUSTER_VERTICES);
    std::vector<unsigned char> mt(max_m * bud::asset::VG_MAX_CLUSTER_TRIANGLES * 3);

    size_t count = meshopt_buildMeshletsScan(meshlets.data(), mv.data(), mt.data(),
                                             cached.data(), cached.size(),
                                             deduped_v.size(),
                                             bud::asset::VG_MAX_CLUSTER_VERTICES, bud::asset::VG_MAX_CLUSTER_TRIANGLES);

    std::vector<InternalCluster> split_res;
    for (size_t i = 0; i < count; ++i) {
        auto& m = meshlets[i];
        meshopt_optimizeMeshlet(&mv[m.vertex_offset], &mt[m.triangle_offset], m.triangle_count, m.vertex_count);

        InternalCluster sc;
        sc.material_index = material_index;
        sc.level = level;
        sc.vertices.reserve(m.vertex_count);
        sc.indices.reserve(m.triangle_count * 3);

        for (uint32_t v = 0; v < m.vertex_count; ++v)
            sc.vertices.push_back(deduped_v[mv[m.vertex_offset + v]]);
        for (uint32_t t = 0; t < m.triangle_count * 3; ++t)
            sc.indices.push_back(mt[m.triangle_offset + t]);

        sc.lod_error = final_lod_error;
        compute_cluster_bounds(sc);
        split_res.push_back(std::move(sc));
    }
    return split_res;
}

void build_mesh_vg_dag(
    const std::vector<bud::asset::Vertex>& vertices,
    const std::vector<uint32_t>& indices,
    uint32_t material_index,
    std::vector<InternalCluster>& out_clusters,
    std::vector<InternalGroup>& out_groups) {

    if (vertices.empty() || indices.empty())
        return;

    float origin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float ext[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const auto& v : vertices) {
        for (int k = 0; k < 3; ++k) {
            origin[k] = std::min(origin[k], v.position[k]);
            ext[k] = std::max(ext[k], v.position[k]);
        }
    }
    float scale[3];
    for (int k = 0; k < 3; ++k)
        scale[k] = (ext[k] > origin[k]) ? (1023.0f / (ext[k] - origin[k])) : 0.0f;

    const uint32_t l0_cluster_start = static_cast<uint32_t>(out_clusters.size());
    generate_leaf_clusters(vertices, indices, material_index, out_clusters);

    const uint32_t l0_cluster_count = static_cast<uint32_t>(out_clusters.size()) - l0_cluster_start;
    if (l0_cluster_count == 0)
        return;

    const uint32_t l0_group_start = static_cast<uint32_t>(out_groups.size());

    // Single cluster case: single group, done
    if (l0_cluster_count == 1) {
        InternalGroup g;
        g.level = 0;
        g.cluster_start = l0_cluster_start;
        g.cluster_count = 1;
        g.children_start = 0;
        g.children_count = 0;
        g.lod_bounds_center[0] = out_clusters[l0_cluster_start].lod_bounds_center[0];
        g.lod_bounds_center[1] = out_clusters[l0_cluster_start].lod_bounds_center[1];
        g.lod_bounds_center[2] = out_clusters[l0_cluster_start].lod_bounds_center[2];
        g.lod_bounds_radius = out_clusters[l0_cluster_start].lod_bounds_radius;
        g.lod_error = out_clusters[l0_cluster_start].lod_error;
        out_clusters[l0_cluster_start].group_index = l0_group_start;
        out_groups.push_back(g);
        return;
    }

    // Step 1: Partition level 0 clusters into groups of up to 4 clusters using METIS / Morton order
    std::unordered_map<uint64_t, std::vector<uint32_t>> edge_to_clusters;
    for (uint32_t cluster_idx = l0_cluster_start; cluster_idx < out_clusters.size(); ++cluster_idx) {
        const auto& cluster = out_clusters[cluster_idx];
        for (size_t i = 0; i < cluster.indices.size(); i += 3) {
            auto get_hash = [](const bud::asset::Vertex& v) -> uint32_t {
                uint32_t hx = *reinterpret_cast<const uint32_t*>(&v.position[0]);
                uint32_t hy = *reinterpret_cast<const uint32_t*>(&v.position[1]);
                uint32_t hz = *reinterpret_cast<const uint32_t*>(&v.position[2]);
                return hx ^ (hy * 16777619u) ^ (hz * 2166136261u);
            };

            uint32_t v0 = get_hash(cluster.vertices[cluster.indices[i]]);
            uint32_t v1 = get_hash(cluster.vertices[cluster.indices[i + 1]]);
            uint32_t v2 = get_hash(cluster.vertices[cluster.indices[i + 2]]);

            auto add_edge = [&](uint32_t a, uint32_t b) {
                uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
                edge_to_clusters[key].push_back(cluster_idx);
            };
            add_edge(v0, v1);
            add_edge(v1, v2);
            add_edge(v2, v0);
        }
    }

    std::vector<std::unordered_map<uint32_t, uint32_t>> cluster_adjacency(l0_cluster_count);
    for (const auto& [edge_key, adjacent_clusters] : edge_to_clusters) {
        if (adjacent_clusters.size() == 2) {
            uint32_t c0 = adjacent_clusters[0] - l0_cluster_start;
            uint32_t c1 = adjacent_clusters[1] - l0_cluster_start;
            cluster_adjacency[c0][c1]++;
            cluster_adjacency[c1][c0]++;
        }
    }

    std::vector<idx_t> xadj;
    std::vector<idx_t> adjncy;
    std::vector<idx_t> adjwgt;
    xadj.push_back(0);
    for (uint32_t i = 0; i < l0_cluster_count; ++i) {
        for (const auto& [neighbor, weight] : cluster_adjacency[i]) {
            adjncy.push_back(static_cast<idx_t>(neighbor));
            adjwgt.push_back(static_cast<idx_t>(weight));
        }
        xadj.push_back(static_cast<idx_t>(adjncy.size()));
    }

    idx_t nvtxs = static_cast<idx_t>(l0_cluster_count);
    idx_t ncon = 1;
    const uint32_t max_clusters_per_group = 4;
    idx_t nparts = static_cast<idx_t>((l0_cluster_count + max_clusters_per_group - 1) / max_clusters_per_group);

    std::vector<idx_t> part(l0_cluster_count, 0);
    idx_t objval = 0;

    if (nparts > 1) {
        idx_t options[METIS_NOPTIONS];
        METIS_SetDefaultOptions(options);
        options[METIS_OPTION_CONTIG] = 0;
        options[METIS_OPTION_OBJTYPE] = METIS_OBJTYPE_CUT;
        options[METIS_OPTION_UFACTOR] = 1;

        METIS_PartGraphKway(
            &nvtxs, &ncon, xadj.data(), adjncy.data(),
            nullptr, nullptr, adjwgt.data(), &nparts, nullptr,
            nullptr, options, &objval, part.data()
        );
    }

    std::map<idx_t, std::vector<uint32_t>> grouped_indices;
    for (uint32_t i = 0; i < l0_cluster_count; ++i) {
        grouped_indices[part[i]].push_back(l0_cluster_start + i);
    }

    struct HardwareChunkData {
        std::vector<uint32_t> global_cluster_indices;
        float chunk_centroid[3] = { 0.0f, 0.0f, 0.0f };
        uint32_t morton_code = 0;
    };
    std::vector<HardwareChunkData> hardware_chunks;

    for (auto& [part_id, cluster_indices] : grouped_indices) {
        for (size_t chunk_start = 0; chunk_start < cluster_indices.size(); chunk_start += max_clusters_per_group) {
            HardwareChunkData chunk;
            size_t chunk_end = std::min(chunk_start + max_clusters_per_group, cluster_indices.size());

            for (size_t k = chunk_start; k < chunk_end; ++k) {
                chunk.global_cluster_indices.push_back(cluster_indices[k]);
                for (int axis = 0; axis < 3; ++axis)
                    chunk.chunk_centroid[axis] += out_clusters[cluster_indices[k]].lod_bounds_center[axis];
            }

            for (int axis = 0; axis < 3; ++axis)
                chunk.chunk_centroid[axis] /= static_cast<float>(chunk.global_cluster_indices.size());
            hardware_chunks.push_back(std::move(chunk));
        }
    }

    for (auto& chunk : hardware_chunks)
        chunk.morton_code = compute_morton_code_3d(chunk.chunk_centroid, origin, scale);

    std::sort(hardware_chunks.begin(), hardware_chunks.end(), [](const HardwareChunkData& a, const HardwareChunkData& b) {
        return a.morton_code < b.morton_code;
    });

    std::vector<InternalCluster> reordered_l0;
    reordered_l0.reserve(l0_cluster_count);

    for (const auto& chunk : hardware_chunks) {
        InternalGroup g;
        g.level = 0;
        g.cluster_start = l0_cluster_start + static_cast<uint32_t>(reordered_l0.size());
        g.cluster_count = static_cast<uint32_t>(chunk.global_cluster_indices.size());
        g.children_start = 0;
        g.children_count = 0;

        for (uint32_t original_idx : chunk.global_cluster_indices)
            reordered_l0.push_back(std::move(out_clusters[original_idx]));
        out_groups.push_back(g);
    }

    for (uint32_t i = 0; i < l0_cluster_count; ++i)
        out_clusters[l0_cluster_start + i] = std::move(reordered_l0[i]);

    const uint32_t l0_group_count = static_cast<uint32_t>(out_groups.size()) - l0_group_start;

    // Build cross-group shared edge map for seam prevention.
    // An edge shared between two clusters in different L0 groups must be locked
    // during simplification. This prevents cracks when adjacent groups are at
    // different LOD levels.
    std::unordered_set<uint64_t> cross_group_edges;
    {
        // Build group index lookup for each cluster
        std::vector<uint32_t> cluster_group(out_clusters.size(), ~0u);
        for (uint32_t g = 0; g < l0_group_count; ++g) {
            auto& grp = out_groups[l0_group_start + g];
            for (uint32_t k = 0; k < grp.cluster_count; ++k)
                cluster_group[grp.cluster_start + k] = l0_group_start + g;
        }
        for (const auto& [edge_key, adjacent_clusters] : edge_to_clusters) {
            std::unordered_set<uint32_t> group_set;
            for (uint32_t ci : adjacent_clusters) {
                if (ci < cluster_group.size() && cluster_group[ci] != ~0u)
                    group_set.insert(cluster_group[ci]);
            }
            // If this edge is shared between two different groups, it's a cross-group edge
            if (group_set.size() > 1)
                cross_group_edges.insert(edge_key);
        }
    }

    for (uint32_t g = 0; g < l0_group_count; ++g) {
        auto& grp = out_groups[l0_group_start + g];
        for (uint32_t k = 0; k < grp.cluster_count; ++k)
            out_clusters[grp.cluster_start + k].group_index = l0_group_start + g;

        float max_error = 0.0f;
        float center[3] = { 0.0f, 0.0f, 0.0f };
        float total_weight = 0.0f;

        for (uint32_t k = 0; k < grp.cluster_count; ++k) {
            const auto& c = out_clusters[grp.cluster_start + k];
            max_error = std::max(max_error, c.lod_error);
            for (int axis = 0; axis < 3; ++axis)
                center[axis] += c.lod_bounds_center[axis];
            total_weight += 1.0f;
        }

        if (total_weight > 0.0f) {
            for (int axis = 0; axis < 3; ++axis)
                center[axis] /= total_weight;
        }

        float max_extent = 0.0f;
        for (uint32_t k = 0; k < grp.cluster_count; ++k) {
            const auto& c = out_clusters[grp.cluster_start + k];
            float dx = c.lod_bounds_center[0] - center[0];
            float dy = c.lod_bounds_center[1] - center[1];
            float dz = c.lod_bounds_center[2] - center[2];
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz) + c.lod_bounds_radius;
            max_extent = std::max(max_extent, dist);
        }

        grp.lod_bounds_center[0] = center[0];
        grp.lod_bounds_center[1] = center[1];
        grp.lod_bounds_center[2] = center[2];
        grp.lod_bounds_radius = max_extent;
        grp.lod_error = max_error;
    }

    // Step 2: Hierarchical group reduction upwards until 1 root group remains
    uint32_t current_level_group_start = l0_group_start;
    uint32_t current_level_group_count = l0_group_count;
    uint32_t level = 0;

    while (current_level_group_count > 1 && level < 16) {
        const uint32_t max_children = 4;
        uint32_t next_level_group_start = static_cast<uint32_t>(out_groups.size());
        uint32_t next_level_group_count = 0;

        for (uint32_t g = 0; g < current_level_group_count; g += max_children) {
            uint32_t count = std::min(max_children, current_level_group_count - g);
            uint32_t child_start_idx = current_level_group_start + g;

            // Collect all clusters from these 'count' child groups
            std::vector<uint32_t> child_clusters;
            float center[3] = { 0.0f, 0.0f, 0.0f };
            float max_error = 0.0f;

            for (uint32_t k = 0; k < count; ++k) {
                const auto& cg = out_groups[child_start_idx + k];
                max_error = std::max(max_error, cg.lod_error);
                center[0] += cg.lod_bounds_center[0];
                center[1] += cg.lod_bounds_center[1];
                center[2] += cg.lod_bounds_center[2];
                for (uint32_t j = 0; j < cg.cluster_count; ++j) {
                    child_clusters.push_back(cg.cluster_start + j);
                }
            }
            center[0] /= static_cast<float>(count);
            center[1] /= static_cast<float>(count);
            center[2] /= static_cast<float>(count);

            float max_r = 0.0f;
            for (uint32_t k = 0; k < count; ++k) {
                const auto& cg = out_groups[child_start_idx + k];
                float dx = cg.lod_bounds_center[0] - center[0];
                float dy = cg.lod_bounds_center[1] - center[1];
                float dz = cg.lod_bounds_center[2] - center[2];
                float dist = std::sqrt(dx * dx + dy * dy + dz * dz) + cg.lod_bounds_radius;
                max_r = std::max(max_r, dist);
            }

            // Simplify the combined geometry of ALL 'count' child groups
            std::vector<InternalCluster> simplified = simplify_cluster_set(
                child_clusters, out_clusters, level + 1, material_index, max_r, &cross_group_edges);

            uint32_t parent_cluster_start = static_cast<uint32_t>(out_clusters.size());
            uint32_t parent_cluster_count = static_cast<uint32_t>(simplified.size());
            for (auto& sc : simplified) {
                sc.source_group = child_start_idx;
                max_error = std::max(max_error, sc.lod_error);
                out_clusters.push_back(std::move(sc));
            }

            InternalGroup parent_grp{};
            parent_grp.level = level + 1;
            parent_grp.cluster_start = parent_cluster_start;
            parent_grp.cluster_count = parent_cluster_count;
            parent_grp.children_start = child_start_idx;
            parent_grp.children_count = count;
            parent_grp.lod_bounds_center[0] = center[0];
            parent_grp.lod_bounds_center[1] = center[1];
            parent_grp.lod_bounds_center[2] = center[2];
            parent_grp.lod_bounds_radius = max_r;
            parent_grp.lod_error = max_error;

            uint32_t parent_idx = static_cast<uint32_t>(out_groups.size());
            for (uint32_t k = 0; k < parent_cluster_count; ++k) {
                out_clusters[parent_cluster_start + k].group_index = parent_idx;
            }

            out_groups.push_back(parent_grp);
            next_level_group_count++;
        }

        level++;
        current_level_group_start = next_level_group_start;
        current_level_group_count = next_level_group_count;
    }

    // Connect parent LOD error downwards for monotonic DAG evaluation
    for (uint32_t g = 0; g < out_groups.size(); ++g) {
        const auto& grp = out_groups[g];
        float parent_error = grp.lod_error;

        for (uint32_t k = 0; k < grp.children_count; ++k) {
            uint32_t child_group_idx = grp.children_start + k;
            if (child_group_idx < out_groups.size()) {
                const auto& cg = out_groups[child_group_idx];
                for (uint32_t j = 0; j < cg.cluster_count; ++j) {
                    uint32_t child_cluster_idx = cg.cluster_start + j;
                    if (child_cluster_idx < out_clusters.size()) {
                        out_clusters[child_cluster_idx].parent_lod_error = std::max(out_clusters[child_cluster_idx].parent_lod_error, parent_error);
                    }
                }
            }
        }
    }

    // Root clusters assign parent_lod_error = lod_error
    for (auto& c : out_clusters) {
        if (c.group_index == bud::asset::INVALID_INDEX)
            c.parent_lod_error = c.lod_error;
    }
}

void assign_vg_pages(
    const std::vector<InternalCluster>& clusters,
    const std::vector<InternalGroup>& groups,
    std::vector<InternalPage>& out_pages,
    std::vector<uint32_t>& out_cluster_page,
    std::vector<uint32_t>& out_cluster_page_vertex_offset,
    std::vector<uint32_t>& out_cluster_page_index_offset) {

    const uint32_t header_size = sizeof(bud::asset::VGPageDataHeader);
    const uint32_t capacity = bud::asset::VG_PAGE_SIZE;

    out_pages.clear();
    out_cluster_page.assign(clusters.size(), bud::asset::INVALID_INDEX);
    out_cluster_page_vertex_offset.assign(clusters.size(), 0);
    out_cluster_page_index_offset.assign(clusters.size(), 0);

    uint32_t page_id = bud::asset::INVALID_INDEX;
    uint32_t page_vertex_count = 0;
    uint32_t page_tri_count = 0;
    std::vector<uint32_t> page_clusters;

    auto calc_raw_bytes = [&](uint32_t verts, uint32_t tris, uint32_t cluster_count, uint32_t bits = 16u) -> uint32_t {
        const uint32_t pos_bytes = static_cast<uint32_t>((static_cast<uint64_t>(verts) * bits * 3 + 7) / 8);
        const uint32_t pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
        const uint32_t attr_bytes = verts * static_cast<uint32_t>(sizeof(bud::asset::VGPackedVertex));
        const uint32_t idx_bytes = tris * 3u * static_cast<uint32_t>(sizeof(uint16_t));
        return header_size + cluster_count * kClusterGPUOverhead + pos_bytes_aligned + attr_bytes + idx_bytes;
    };

    auto close_page = [&]() {
        if (page_id == bud::asset::INVALID_INDEX)
            return;
        uint32_t cc = static_cast<uint32_t>(page_clusters.size());
        uint32_t min_lvl = UINT32_MAX;
        for (uint32_t ci : page_clusters)
            min_lvl = std::min(min_lvl, clusters[ci].level);
        out_pages[page_id].lod_level = (min_lvl == UINT32_MAX) ? 0 : min_lvl;
        out_pages[page_id].clusters = std::move(page_clusters);
        out_pages[page_id].vertex_count = page_vertex_count;
        out_pages[page_id].index_count = page_tri_count;
        out_pages[page_id].size_in_bytes = calc_raw_bytes(page_vertex_count, page_tri_count, cc);
    };

    uint32_t current_page_level = bud::asset::INVALID_INDEX;

    for (uint32_t ci = 0; ci < static_cast<uint32_t>(clusters.size()); ++ci) {
        const uint32_t cv = static_cast<uint32_t>(clusters[ci].vertices.size());
        const uint32_t ct = static_cast<uint32_t>(clusters[ci].indices.size()) / 3;

        const uint32_t cand_verts = page_vertex_count + cv;
        const uint32_t cand_tris = page_tri_count + ct;
        const uint32_t cand_clusters = static_cast<uint32_t>(page_clusters.size()) + 1u;
        const uint32_t cand_bytes = calc_raw_bytes(cand_verts, cand_tris, cand_clusters);

        const bool level_changed = (current_page_level != bud::asset::INVALID_INDEX && current_page_level != clusters[ci].level);

        if (page_id == bud::asset::INVALID_INDEX || cand_bytes > capacity || level_changed) {
            close_page();
            out_pages.push_back({});
            page_id = static_cast<uint32_t>(out_pages.size()) - 1;
            page_vertex_count = 0;
            page_tri_count = 0;
            page_clusters.clear();
            current_page_level = clusters[ci].level;
        }
        out_cluster_page[ci] = page_id;
        out_cluster_page_vertex_offset[ci] = page_vertex_count;
        out_cluster_page_index_offset[ci] = page_tri_count;
        page_clusters.push_back(ci);
        page_vertex_count += cv;
        page_tri_count += ct;
    }
    close_page();

    std::vector<uint32_t> cluster_parent(clusters.size(), bud::asset::INVALID_INDEX);
    for (uint32_t ci = 0; ci < static_cast<uint32_t>(clusters.size()); ++ci) {
        const uint32_t sg = clusters[ci].source_group;
        if (sg == bud::asset::INVALID_INDEX || sg >= groups.size())
            continue;
        for (uint32_t k = 0; k < groups[sg].cluster_count; ++k) {
            const uint32_t child = groups[sg].cluster_start + k;
            if (child < clusters.size())
                cluster_parent[child] = ci;
        }
    }

    for (uint32_t pi = 0; pi < static_cast<uint32_t>(out_pages.size()); ++pi) {
        auto& page = out_pages[pi];
        // Parent / root level pages (LOD >= 1) serve as resident base representations.
        // Leaf pages (LOD == 0) are dynamically streamed based on camera proximity.
        page.is_root = (page.lod_level > 0);
        page.dependency_page_id = page.is_root ? bud::asset::INVALID_INDEX : 0;
    }
}

void write_page_quantized_positions(const std::vector<bud::asset::Vertex>& page_vertices,
                                    const float offset[3], const float extent[3],
                                    uint32_t bits, std::vector<uint8_t>& out) {
    const uint32_t max_q = (1u << bits) - 1;
    const size_t total_bits = page_vertices.size() * 3ull * bits;
    out.assign((total_bits + 7) / 8, 0);
    size_t bit_pos = 0;

    for (const auto& v : page_vertices) {
        for (int k = 0; k < 3; ++k) {
            const float span = extent[k];
            const float t = (span > 0.0f) ? ((v.position[k] - offset[k]) / span) : 0.0f;
            const uint32_t q = static_cast<uint32_t>(std::clamp(t, 0.0f, 1.0f) * static_cast<float>(max_q) + 0.5f);
            for (uint32_t b = 0; b < bits; ++b) {
                if (q & (1u << b)) {
                    const size_t byte = bit_pos >> 3;
                    const size_t bit = bit_pos & 7;
                    out[byte] |= static_cast<uint8_t>(1u << bit);
                }
                ++bit_pos;
            }
        }
    }
}

bud::asset::VGPackedVertex pack_vertex_attributes(const bud::asset::Vertex& v) {
    bud::asset::VGPackedVertex p{};
    auto pack_snorm8 = [](float x) -> uint8_t {
        return static_cast<uint8_t>(static_cast<int8_t>(std::clamp(x, -1.0f, 1.0f) * 127.0f));
    };
    auto pack_u16 = [](float x) -> uint16_t {
        return meshopt_quantizeHalf(x);
    };

    p.normal[0] = pack_snorm8(v.normal[0]);
    p.normal[1] = pack_snorm8(v.normal[1]);
    p.normal[2] = pack_snorm8(v.normal[2]);
    p.normal[3] = 0;

    p.tangent[0] = pack_snorm8(v.tangent[0]);
    p.tangent[1] = pack_snorm8(v.tangent[1]);
    p.tangent[2] = pack_snorm8(v.tangent[2]);
    p.tangent[3] = pack_snorm8(v.tangent[3]);

    p.uv[0] = pack_u16(v.uv[0]);
    p.uv[1] = pack_u16(v.uv[1]);
    p.color = 0;
    return p;
}

} // namespace

VGBuildResult VirtualGeometryBuilder::build(const InternalMesh& mesh) {
    VGBuildResult result;

    std::vector<InternalCluster> all_clusters;
    std::vector<InternalGroup> all_groups;

    std::vector<uint32_t> submesh_root_groups;

    for (const auto& submesh : mesh.submeshes) {
        if (submesh.vertices.empty() || submesh.indices.empty())
            continue;

        uint32_t cluster_base = static_cast<uint32_t>(all_clusters.size());
        uint32_t group_base = static_cast<uint32_t>(all_groups.size());

        std::vector<InternalCluster> submesh_clusters;
        std::vector<InternalGroup> submesh_groups;

        build_mesh_vg_dag(submesh.vertices, submesh.indices, submesh.material_index, submesh_clusters, submesh_groups);

        for (auto& c : submesh_clusters) {
            if (c.group_index != bud::asset::INVALID_INDEX)
                c.group_index += group_base;
            if (c.source_group != bud::asset::INVALID_INDEX)
                c.source_group += group_base;
            all_clusters.push_back(std::move(c));
        }

        for (auto& g : submesh_groups) {
            g.cluster_start += cluster_base;
            if (g.children_count > 0)
                g.children_start += group_base;
            all_groups.push_back(std::move(g));
        }

        if (!submesh_groups.empty()) {
            submesh_root_groups.push_back(group_base + static_cast<uint32_t>(submesh_groups.size()) - 1);
        }
    }

    if (all_clusters.empty())
        return result;

    // Super-Root Hierarchy Assembly:
    // When an asset contains multiple submeshes (e.g. Sponza with 393 submeshes),
    // synthesize a hierarchical Super-Root tree so that GPU traversal starts from
    // a single unified root group.
    uint32_t final_root_group_index = 0;
    if (submesh_root_groups.size() == 1) {
        final_root_group_index = submesh_root_groups[0];
    }
    else if (submesh_root_groups.size() > 1) {
        // Step 1: Copy submesh root group headers into all_groups contiguously
        uint32_t contiguous_base = static_cast<uint32_t>(all_groups.size());
        for (uint32_t r_idx : submesh_root_groups) {
            all_groups.push_back(all_groups[r_idx]);
        }

        std::vector<uint32_t> current_level_roots(submesh_root_groups.size());
        for (size_t i = 0; i < submesh_root_groups.size(); ++i) {
            current_level_roots[i] = contiguous_base + static_cast<uint32_t>(i);
        }

        const uint32_t max_children = 16;
        while (current_level_roots.size() > 1) {
            std::vector<uint32_t> next_level_roots;
            for (size_t start = 0; start < current_level_roots.size(); start += max_children) {
                uint32_t count = static_cast<uint32_t>(std::min<size_t>(max_children, current_level_roots.size() - start));
                uint32_t child_start_idx = current_level_roots[start];

                InternalGroup super_grp{};
                super_grp.cluster_start = 0;
                super_grp.cluster_count = 0;
                super_grp.children_start = child_start_idx;
                super_grp.children_count = count;
                super_grp.lod_error = FLT_MAX; // Always expands into children

                // Compute bounding sphere encompassing all children
                float center[3] = { 0.0f, 0.0f, 0.0f };
                for (uint32_t k = 0; k < count; ++k) {
                    const auto& child = all_groups[child_start_idx + k];
                    center[0] += child.lod_bounds_center[0];
                    center[1] += child.lod_bounds_center[1];
                    center[2] += child.lod_bounds_center[2];
                }
                center[0] /= static_cast<float>(count);
                center[1] /= static_cast<float>(count);
                center[2] /= static_cast<float>(count);

                float max_r = 0.0f;
                for (uint32_t k = 0; k < count; ++k) {
                    const auto& child = all_groups[child_start_idx + k];
                    float dx = child.lod_bounds_center[0] - center[0];
                    float dy = child.lod_bounds_center[1] - center[1];
                    float dz = child.lod_bounds_center[2] - center[2];
                    float dist = std::sqrt(dx * dx + dy * dy + dz * dz) + child.lod_bounds_radius;
                    max_r = std::max(max_r, dist);
                }

                super_grp.lod_bounds_center[0] = center[0];
                super_grp.lod_bounds_center[1] = center[1];
                super_grp.lod_bounds_center[2] = center[2];
                super_grp.lod_bounds_radius = max_r;

                uint32_t super_idx = static_cast<uint32_t>(all_groups.size());
                all_groups.push_back(super_grp);
                next_level_roots.push_back(super_idx);
            }
            current_level_roots = std::move(next_level_roots);
        }
        final_root_group_index = current_level_roots[0];
    }

    std::vector<InternalPage> pages;
    std::vector<uint32_t> cluster_page;
    std::vector<uint32_t> cluster_page_vertex_offset;
    std::vector<uint32_t> cluster_page_index_offset;

    assign_vg_pages(all_clusters, all_groups, pages, cluster_page, cluster_page_vertex_offset, cluster_page_index_offset);

    // 1:1 UE5 VGCluster serialization
    result.clusters.resize(all_clusters.size());
    for (size_t ci = 0; ci < all_clusters.size(); ++ci) {
        const auto& cb = all_clusters[ci];
        auto& sc = result.clusters[ci];
        sc.num_verts = static_cast<uint32_t>(cb.vertices.size());
        sc.num_tris = static_cast<uint32_t>(cb.indices.size() / 3);
        sc.material_index = cb.material_index;
        sc.position_offset = cluster_page_vertex_offset[ci];
        sc.position_page_offset = cluster_page[ci];
        sc.index_offset = cluster_page_index_offset[ci];
        sc.index_page_offset = cluster_page[ci];
        sc.group_index = cb.group_index;
        sc.lod_error = bud::asset::vg_encode_lod_error(cb.lod_error);
        sc.parent_lod_error = bud::asset::vg_encode_lod_error(cb.parent_lod_error);

        for (int k = 0; k < 3; ++k) {
            sc.position_bounds_center[k] = (cb.bounds_min[k] + cb.bounds_max[k]) * 0.5f;
            sc.position_bounds_extent[k] = (cb.bounds_max[k] - cb.bounds_min[k]) * 0.5f;
            sc.lod_bounds_center[k] = cb.lod_bounds_center[k];
        }
        sc.lod_bounds_radius = cb.lod_bounds_radius;
        sc.cone_axis[0] = cb.cone_axis[0];
        sc.cone_axis[1] = cb.cone_axis[1];
        sc.cone_axis[2] = cb.cone_axis[2];
        sc.cone_cutoff = cb.cone_cutoff;
    }

    // 1:1 UE5 VGClusterGroup serialization
    result.groups.resize(all_groups.size());
    for (size_t gi = 0; gi < all_groups.size(); ++gi) {
        const auto& gb = all_groups[gi];
        auto& sg = result.groups[gi];
        uint32_t pmin = ~0u, pmax = 0;
        for (uint32_t k = 0; k < gb.cluster_count; ++k) {
            const uint32_t p = cluster_page[gb.cluster_start + k];
            pmin = std::min(pmin, p);
            pmax = std::max(pmax, p);
        }
        sg.page_index_start = (pmin == ~0u) ? bud::asset::INVALID_INDEX : pmin;
        sg.page_index_num = (pmin == ~0u) ? 0 : (pmax - pmin + 1);
        sg.children_start = gb.children_start;
        sg.children_num = gb.children_count;
        sg.lod_bounds_center[0] = gb.lod_bounds_center[0];
        sg.lod_bounds_center[1] = gb.lod_bounds_center[1];
        sg.lod_bounds_center[2] = gb.lod_bounds_center[2];
        sg.lod_bounds_radius = gb.lod_bounds_radius;
        sg.lod_error = gb.lod_error;
    }

    // 1:1 UE5 VGPageDependency serialization
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        const uint32_t dep = pages[pi].dependency_page_id;
        if (dep == bud::asset::INVALID_INDEX || dep >= pages.size())
            continue;

        uint32_t gs = ~0u, ge = 0;
        for (uint32_t ci : pages[dep].clusters) {
            const uint32_t g = result.clusters[ci].group_index;
            if (g != bud::asset::INVALID_INDEX) {
                gs = std::min(gs, g);
                ge = std::max(ge, g);
            }
        }
        if (gs != ~0u) {
            bud::asset::VGPageDependency d{};
            d.page_id = dep;
            d.start_group_index = gs;
            d.num_groups = ge - gs + 1;
            result.dependencies.push_back(d);
        }
    }

    // Assemble 128KB raw binary pages & populate VGPageStreamingState
    result.pages.resize(pages.size());
    result.raw_page_data.resize(pages.size());
    for (size_t pi = 0; pi < pages.size(); ++pi) {
        std::vector<bud::asset::Vertex> page_vertices;
        page_vertices.reserve(pages[pi].vertex_count);
        float poff[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float pext[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

        for (uint32_t ci : pages[pi].clusters) {
            const auto& cb = all_clusters[ci];
            for (const auto& v : cb.vertices) {
                page_vertices.push_back(v);
                for (int k = 0; k < 3; ++k) {
                    poff[k] = std::min(poff[k], v.position[k]);
                    pext[k] = std::max(pext[k], v.position[k]);
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            pext[k] = (pext[k] > poff[k]) ? (pext[k] - poff[k]) : 1.0f;
        }

        // Calculate adaptive quantization precision based on page extent.
        // Target: 0.1cm (1mm) precision. Unit = cm so 0.1 = 1mm.
        // Clamp to 8..16 bits. 16 bits max ensures the page fits in the 128KB slot.
        float max_extent = std::max({ pext[0], pext[1], pext[2] });
        uint32_t bits = 12u; // default
        if (max_extent > 0.0f) {
            bits = std::clamp(static_cast<uint32_t>(std::ceil(std::log2(max_extent / 0.1f))), 8u, 16u);
        }
        std::vector<uint8_t> pos_stream;
        write_page_quantized_positions(page_vertices, poff, pext, bits, pos_stream);

        std::vector<bud::asset::VGPackedVertex> attr_stream;
        attr_stream.reserve(pages[pi].vertex_count);
        for (uint32_t ci : pages[pi].clusters) {
            for (const auto& v : all_clusters[ci].vertices)
                attr_stream.push_back(pack_vertex_attributes(v));
        }

        std::vector<uint16_t> idx_stream;
        idx_stream.reserve(pages[pi].index_count * 3u);
        for (uint32_t ci : pages[pi].clusters) {
            const auto& cb = all_clusters[ci];
            const uint32_t vbase = cluster_page_vertex_offset[ci];
            for (uint32_t idx : cb.indices)
                idx_stream.push_back(static_cast<uint16_t>(vbase + idx));
        }

        const uint32_t pos_bytes = static_cast<uint32_t>(pos_stream.size());
        const uint32_t pos_bytes_aligned = (pos_bytes + 3u) & ~3u;
        const uint32_t attr_bytes = static_cast<uint32_t>(attr_stream.size() * sizeof(bud::asset::VGPackedVertex));
        const uint32_t idx_bytes = static_cast<uint32_t>(idx_stream.size() * sizeof(uint16_t));
        const uint32_t cc = static_cast<uint32_t>(pages[pi].clusters.size());
        const uint32_t overhead = cc * kClusterGPUOverhead;

        const uint32_t actual_page_size = static_cast<uint32_t>(sizeof(bud::asset::VGPageDataHeader)) + overhead + pos_bytes_aligned + attr_bytes + idx_bytes;

        // 1:1 UE5 VGPageStreamingState serialization
        auto& sp = result.pages[pi];
        sp.raw_vertex_offset = sizeof(bud::asset::VGPageDataHeader) + overhead;
        sp.raw_vertex_count = pages[pi].vertex_count;
        sp.raw_index_offset = sizeof(bud::asset::VGPageDataHeader) + overhead + pos_bytes_aligned + attr_bytes;
        sp.raw_index_count = pages[pi].index_count;
        sp.imposter_offset = 0;
        sp.imposter_count = 0;
        sp.flags = pages[pi].lod_level;
        sp.dependency_page_id = pages[pi].dependency_page_id;
        sp.size_in_bytes = actual_page_size;

        bud::asset::VGPageDataHeader pd{};
        pd.magic = bud::asset::VG_PAGE_DATA_MAGIC;
        pd.version = bud::asset::VG_VERSION;
        pd.cluster_count = cc;
        pd.vertex_count = pages[pi].vertex_count;
        pd.index_count = pages[pi].index_count;
        pd.vertex_stream_offset = sp.raw_vertex_offset;
        pd.index_stream_offset = sp.raw_index_offset;
        pd.total_size = actual_page_size;
        pd.flags = pages[pi].is_root ? 1u : 0u;
        pd.position_bits = bits;
        for (int k = 0; k < 3; ++k) {
            pd.position_offset[k] = poff[k];
            pd.position_extent[k] = pext[k];
        }

        auto& blob = result.raw_page_data[pi];
        blob.resize(actual_page_size, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &pd, sizeof(pd));
        dst += sizeof(pd);

        // Write per-cluster GPU descriptors (7 dwords each) and cull data (5 dwords each)
        for (uint32_t ci : pages[pi].clusters) {
            const auto& cb = all_clusters[ci];
            uint32_t desc[7] = {};
            desc[0] = cluster_page_vertex_offset[ci];   // vertex_offset
            desc[1] = static_cast<uint32_t>(cb.vertices.size()); // vertex_count
            desc[2] = cluster_page_index_offset[ci];    // triangle_offset (in triangles)
            desc[3] = static_cast<uint32_t>(cb.indices.size() / 3); // triangle_count
            desc[4] = cb.material_index;                // material_index
            std::memcpy(dst, desc, sizeof(desc));
            dst += sizeof(desc);

            uint32_t cull[5] = {};
            // sphere center = position bounds center
            float sc[3] = {
                (cb.bounds_min[0] + cb.bounds_max[0]) * 0.5f,
                (cb.bounds_min[1] + cb.bounds_max[1]) * 0.5f,
                (cb.bounds_min[2] + cb.bounds_max[2]) * 0.5f,
            };
            std::memcpy(&cull[0], &sc[0], sizeof(float));
            std::memcpy(&cull[1], &sc[1], sizeof(float));
            std::memcpy(&cull[2], &sc[2], sizeof(float));
            // sphere radius = max extent
            float sr = std::max({
                cb.bounds_max[0] - cb.bounds_min[0],
                cb.bounds_max[1] - cb.bounds_min[1],
                cb.bounds_max[2] - cb.bounds_min[2],
            }) * 0.5f;
            std::memcpy(&cull[3], &sr, sizeof(float));
            std::memcpy(dst, cull, sizeof(cull));
            dst += sizeof(cull);
        }

        if (!pos_stream.empty()) {
            std::memcpy(dst, pos_stream.data(), pos_bytes);
            dst += pos_bytes_aligned;
        }
        if (!attr_stream.empty()) {
            std::memcpy(dst, attr_stream.data(), attr_bytes);
            dst += attr_bytes;
        }
        if (!idx_stream.empty()) {
            std::memcpy(dst, idx_stream.data(), idx_bytes);
        }
    }

    // Populate VGHeader
    result.header.magic = bud::asset::VG_MAGIC;
    result.header.version = bud::asset::VG_VERSION;
    result.header.flags = 0;
    result.header.cluster_count = static_cast<uint32_t>(result.clusters.size());
    result.header.group_count = static_cast<uint32_t>(result.groups.size());
    result.header.page_count = static_cast<uint32_t>(result.pages.size());
    result.header.dependency_count = static_cast<uint32_t>(result.dependencies.size());
    result.header.material_count = static_cast<uint32_t>(mesh.materials.size());
    result.header.texture_count = static_cast<uint32_t>(mesh.textures.size());
    result.header.root_group_index = final_root_group_index;

    for (int k = 0; k < 3; ++k) {
        result.header.aabb_min[k] = mesh.aabb_min[k];
        result.header.aabb_max[k] = mesh.aabb_max[k];
    }

    return result;
}

bool VirtualGeometryBuilder::dump_json(const VGBuildResult& result, const InternalMesh& mesh, const std::string& path) {
    nlohmann::json j;

    // 1. Container Root Header Metadata
    j["container_header"] = {
        { "magic", "BUDASSET" },
        { "version", 1 },
        { "asset_type", "Mesh" },
        { "chunk_count", 2 }
    };

    // 2. Container Chunk Table Metadata
    j["chunk_table"] = nlohmann::json::array({
        {
            { "chunk_id", 0 },
            { "chunk_type", "Material" },
            { "flags", "None" },
            { "material_count", mesh.materials.size() }
        },
        {
            { "chunk_id", 1 },
            { "chunk_type", "VirtualGeometry" },
            { "flags", "None" },
            { "vg_magic", "BVGT" },
            { "vg_version", result.header.version }
        }
    });

    // 3. Asset Manifest / Model Top-Level Metadata
    size_t total_vertices = 0;
    size_t total_triangles = 0;
    for (const auto& sm : mesh.submeshes) {
        total_vertices += sm.vertices.size();
        total_triangles += sm.indices.size() / 3;
    }
    j["asset_manifest"] = {
        { "submesh_count", mesh.submeshes.size() },
        { "total_vertices", total_vertices },
        { "total_triangles", total_triangles },
        { "aabb_min", { mesh.aabb_min[0], mesh.aabb_min[1], mesh.aabb_min[2] } },
        { "aabb_max", { mesh.aabb_max[0], mesh.aabb_max[1], mesh.aabb_max[2] } },
        { "textures", mesh.textures }
    };

    // 4. Materials Table Metadata
    j["materials"] = nlohmann::json::array();
    for (size_t mi = 0; mi < mesh.materials.size(); ++mi) {
        const auto& mat = mesh.materials[mi];
        j["materials"].push_back({
            { "material_index", mi },
            { "name", mat.name },
            { "base_color_texture", mat.base_color_texture_path },
            { "alpha_mode", static_cast<int>(mat.alpha_mode) == 0 ? "Opaque" : (static_cast<int>(mat.alpha_mode) == 1 ? "Mask" : "Blend") },
            { "double_sided", mat.double_sided },
            { "alpha_cutoff", mat.alpha_cutoff }
        });
    }

    // 5. Virtual Geometry Subsystem Metadata
    nlohmann::json vg;
    vg["stats"] = {
        { "cluster_count", result.clusters.size() },
        { "group_count", result.groups.size() },
        { "page_count", result.pages.size() },
        { "page_size_kb", 128 },
        { "dependency_count", result.dependencies.size() },
        { "material_count", result.header.material_count },
        { "texture_count", result.header.texture_count },
        { "aabb_min", { result.header.aabb_min[0], result.header.aabb_min[1], result.header.aabb_min[2] } },
        { "aabb_max", { result.header.aabb_max[0], result.header.aabb_max[1], result.header.aabb_max[2] } }
    };

    vg["groups"] = nlohmann::json::array();
    for (size_t gi = 0; gi < result.groups.size(); ++gi) {
        const auto& g = result.groups[gi];
        vg["groups"].push_back({
            { "group_index", gi },
            { "page_index_start", g.page_index_start },
            { "page_index_num", g.page_index_num },
            { "children_start", g.children_start },
            { "children_num", g.children_num },
            { "lod_bounds_center", { g.lod_bounds_center[0], g.lod_bounds_center[1], g.lod_bounds_center[2] } },
            { "lod_bounds_radius", g.lod_bounds_radius },
            { "lod_error", g.lod_error }
        });
    }

    vg["clusters"] = nlohmann::json::array();
    for (size_t ci = 0; ci < result.clusters.size(); ++ci) {
        const auto& c = result.clusters[ci];
        vg["clusters"].push_back({
            { "cluster_index", ci },
            { "num_verts", c.num_verts },
            { "num_tris", c.num_tris },
            { "material_index", c.material_index },
            { "group_index", c.group_index },
            { "position_page_offset", c.position_page_offset },
            { "position_offset", c.position_offset },
            { "index_page_offset", c.index_page_offset },
            { "index_offset", c.index_offset },
            { "lod_error", bud::asset::vg_decode_lod_error(c.lod_error) },
            { "parent_lod_error", bud::asset::vg_decode_lod_error(c.parent_lod_error) },
            { "bounds_center", { c.position_bounds_center[0], c.position_bounds_center[1], c.position_bounds_center[2] } },
            { "bounds_extent", { c.position_bounds_extent[0], c.position_bounds_extent[1], c.position_bounds_extent[2] } },
            { "lod_center", { c.lod_bounds_center[0], c.lod_bounds_center[1], c.lod_bounds_center[2] } },
            { "lod_radius", c.lod_bounds_radius },
            { "cone_axis", { c.cone_axis[0], c.cone_axis[1], c.cone_axis[2] } },
            { "cone_cutoff", c.cone_cutoff }
        });
    }

    vg["pages"] = nlohmann::json::array();
    for (size_t pi = 0; pi < result.pages.size(); ++pi) {
        const auto& p = result.pages[pi];
        bool is_streamable = (p.flags & bud::asset::VG_PAGE_FLAG_STREAMABLE) != 0;
        vg["pages"].push_back({
            { "page_index", pi },
            { "storage", is_streamable ? "budbulk" : "inline" },
            { "is_streamable", is_streamable },
            { "bulk_offset", p.raw_vertex_offset },
            { "raw_vertex_count", p.raw_vertex_count },
            { "raw_index_count", p.raw_index_count },
            { "size_in_bytes", p.size_in_bytes },
            { "flags", p.flags },
            { "dependency_page_id", p.dependency_page_id }
        });
    }

    j["virtual_geometry"] = std::move(vg);

    std::ofstream out(path);
    if (!out.is_open())
        return false;

    out << j.dump(2);
    return out.good();
}

} // namespace bud::asset_pipeline
