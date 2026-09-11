#include "cloth_baker.hpp"
#include "src/tools/asset_pipeline/core/support.hpp"
#include <meshoptimizer.h>

#include <unordered_map>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace bud::asset_pipeline {

namespace {

constexpr float kPositionWeldThreshold = 0.001f; // 1mm spatial weld threshold
constexpr float kPositionWeldGridCell = 0.001f;
constexpr float kPinningTopFraction = 0.15f;     // Top 15% is pinning transition
constexpr float kAbsolutePinMargin = 0.05f;     // Top 5% is absolute fixed pin
constexpr float kWarpCompliance = 1e-5f;
constexpr float kWeftCompliance = 2e-5f;
constexpr float kShearCompliance = 6e-4f;
constexpr float kBendingCompliance = 2.5e-3f;
constexpr float kLod1DecimateRatio = 0.40f;      // 40% triangles for LOD1
constexpr float kLod1MaxError = 0.08f;           // 8cm max error for LOD1

struct GridKey {
    int64_t x;
    int64_t y;
    int64_t z;

    bool operator==(const GridKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct GridKeyHash {
    size_t operator()(const GridKey& k) const {
        size_t h1 = std::hash<int64_t>()(k.x);
        size_t h2 = std::hash<int64_t>()(k.y);
        size_t h3 = std::hash<int64_t>()(k.z);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

// Christer Ericson's Real-Time Collision Detection Section 5.1.5
bud::math::vec3 closest_point_on_triangle(
    const bud::math::vec3& p,
    const bud::math::vec3& a,
    const bud::math::vec3& b,
    const bud::math::vec3& c,
    float& out_u, float& out_v, float& out_w)
{
    bud::math::vec3 ab = b - a;
    bud::math::vec3 ac = c - a;
    bud::math::vec3 ap = p - a;

    float d1 = bud::math::dot(ab, ap);
    float d2 = bud::math::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        out_u = 1.0f;
        out_v = 0.0f;
        out_w = 0.0f;
        return a;
    }

    bud::math::vec3 bp = p - b;
    float d3 = bud::math::dot(ab, bp);
    float d4 = bud::math::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        out_u = 0.0f;
        out_v = 1.0f;
        out_w = 0.0f;
        return b;
    }

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        out_u = 1.0f - v;
        out_v = v;
        out_w = 0.0f;
        return a + ab * v;
    }

    bud::math::vec3 cp = p - c;
    float d5 = bud::math::dot(ab, cp);
    float d6 = bud::math::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        out_u = 0.0f;
        out_v = 0.0f;
        out_w = 1.0f;
        return c;
    }

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        out_u = 1.0f - w;
        out_v = 0.0f;
        out_w = w;
        return a + ac * w;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        out_u = 0.0f;
        out_v = 1.0f - w;
        out_w = w;
        return b + (c - b) * w;
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    out_u = 1.0f - v - w;
    out_v = v;
    out_w = w;
    return a + ab * v + ac * w;
}

struct GeneratedSimMesh {
    std::vector<bud::physics::SimParticle> particles;
    std::vector<bud::physics::DistanceConstraint> constraints;
    std::vector<uint32_t> triangles; // 3 indices per tri
};

GeneratedSimMesh build_sim_mesh(const std::vector<bud::math::vec3>& positions, const std::vector<uint32_t>& indices) {
    GeneratedSimMesh mesh;
    if (positions.empty() || indices.empty())
        return mesh;

    // 1. Calculate height bounds for procedural smoothstep pinning
    float y_min = 1e30f;
    float y_max = -1e30f;
    for (const auto& pos : positions) {
        y_min = std::min(y_min, pos.y);
        y_max = std::max(y_max, pos.y);
    }
    float height_span = std::max(y_max - y_min, 0.01f);
    float y_pin_start = y_max - kPinningTopFraction * height_span;
    float y_pin_fixed = y_max - kAbsolutePinMargin * height_span;

    mesh.particles.resize(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        const auto& pos = positions[i];
        float inv_mass = 1.0f;
        if (pos.y >= y_pin_fixed) {
            inv_mass = 0.0f; // Absolutely fixed pin
        } else if (pos.y > y_pin_start) {
            float t = std::clamp((pos.y - y_pin_start) / (y_max - y_pin_start), 0.0f, 1.0f);
            float w_pin = 3.0f * t * t - 2.0f * t * t * t;
            inv_mass = 1.0f - w_pin;
        }

        mesh.particles[i].position_inv_mass = bud::math::vec4(pos.x, pos.y, pos.z, inv_mass);
        mesh.particles[i].prev_position = mesh.particles[i].position_inv_mass;
    }

    mesh.triangles = indices;

    // 2. Extract structural constraints (unique edges)
    std::set<std::pair<uint32_t, uint32_t>> edge_set;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edge_to_opp_vertices;

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t tri[3] = { indices[i], indices[i + 1], indices[i + 2] };
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0])
            continue;

        for (int e = 0; e < 3; ++e) {
            uint32_t v1 = tri[e];
            uint32_t v2 = tri[(e + 1) % 3];
            uint32_t v_opp = tri[(e + 2) % 3];

            uint32_t p1 = std::min(v1, v2);
            uint32_t p2 = std::max(v1, v2);
            edge_set.insert({ p1, p2 });
            edge_to_opp_vertices[{ p1, p2 }].push_back(v_opp);
        }
    }

    for (const auto& edge : edge_set) {
        uint32_t p1 = edge.first;
        uint32_t p2 = edge.second;
        float len = bud::math::distance(positions[p1], positions[p2]);
        if (len < 1e-6f)
            continue;

        bud::math::vec3 dir = (positions[p2] - positions[p1]) / len;
        float dy = std::abs(dir.y);

        bud::physics::DistanceConstraint c;
        c.p1 = p1;
        c.p2 = p2;
        c.rest_length = len;
        if (dy > 0.82f)
            c.compliance = kWarpCompliance;
        else if (dy < 0.28f)
            c.compliance = kWeftCompliance;
        else
            c.compliance = kShearCompliance;

        mesh.constraints.push_back(c);
    }

    // 3. Extract bending constraints (across shared triangle edges)
    for (const auto& pair : edge_to_opp_vertices) {
        if (pair.second.size() == 2) {
            uint32_t o1 = pair.second[0];
            uint32_t o2 = pair.second[1];
            if (o1 == o2)
                continue;

            uint32_t b1 = std::min(o1, o2);
            uint32_t b2 = std::max(o1, o2);

            float len = bud::math::distance(positions[b1], positions[b2]);
            if (len < 1e-6f)
                continue;

            bud::physics::DistanceConstraint bc;
            bc.p1 = b1;
            bc.p2 = b2;
            bc.rest_length = len;
            bc.compliance = kBendingCompliance;
            mesh.constraints.push_back(bc);
        }
    }

    return mesh;
}

std::vector<bud::physics::ClothSkinBinding> compute_barycentric_bindings(
    const std::vector<RawVertex>& render_vertices,
    const GeneratedSimMesh& sim_mesh)
{
    std::vector<bud::physics::ClothSkinBinding> bindings(render_vertices.size());

    const size_t tri_count = sim_mesh.triangles.size() / 3;
    if (tri_count == 0)
        return bindings;

    for (size_t i = 0; i < render_vertices.size(); ++i) {
        bud::math::vec3 p(render_vertices[i].position[0], render_vertices[i].position[1], render_vertices[i].position[2]);

        float min_dist_sq = 1e30f;
        uint32_t best_tri[3] = { 0, 0, 0 };
        bud::math::vec3 best_uvw(1.0f, 0.0f, 0.0f);
        float best_normal_offset = 0.0f;

        for (size_t t = 0; t < tri_count; ++t) {
            uint32_t idx0 = sim_mesh.triangles[t * 3 + 0];
            uint32_t idx1 = sim_mesh.triangles[t * 3 + 1];
            uint32_t idx2 = sim_mesh.triangles[t * 3 + 2];

            bud::math::vec3 a(sim_mesh.particles[idx0].position_inv_mass);
            bud::math::vec3 b(sim_mesh.particles[idx1].position_inv_mass);
            bud::math::vec3 c(sim_mesh.particles[idx2].position_inv_mass);

            float u = 0.0f, v = 0.0f, w = 0.0f;
            bud::math::vec3 q = closest_point_on_triangle(p, a, b, c, u, v, w);
            float dist_sq = bud::math::distance2(p, q);

            if (dist_sq < min_dist_sq) {
                min_dist_sq = dist_sq;
                best_tri[0] = idx0;
                best_tri[1] = idx1;
                best_tri[2] = idx2;
                best_uvw = bud::math::vec3(u, v, w);

                bud::math::vec3 face_normal = bud::math::cross(b - a, c - a);
                float normal_len = bud::math::length(face_normal);
                if (normal_len > 1e-6f) {
                    face_normal /= normal_len;
                    best_normal_offset = bud::math::dot(p - q, face_normal);
                } else {
                    best_normal_offset = 0.0f;
                }
            }
        }

        bindings[i].sim_tri_idx[0] = best_tri[0];
        bindings[i].sim_tri_idx[1] = best_tri[1];
        bindings[i].sim_tri_idx[2] = best_tri[2];
        bindings[i].render_vertex_idx = static_cast<uint32_t>(i);
        bindings[i].barycentric_coords_offset = bud::math::vec4(best_uvw.x, best_uvw.y, best_uvw.z, best_normal_offset);
    }

    return bindings;
}

} // namespace

bool ClothBaker::is_cloth_material(std::string_view name) {
    std::string lower(name);
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (lower.find("fabric") != std::string::npos ||
        lower.find("curtain") != std::string::npos ||
        lower.find("cloth") != std::string::npos ||
        lower.find("banner") != std::string::npos)
        return true;

    for (int i = 282; i <= 289; ++i) {
        if (lower.find("sponza_" + std::to_string(i)) != std::string::npos)
            return true;
    }

    for (int i = 320; i <= 329; ++i) {
        if (lower.find("sponza_" + std::to_string(i)) != std::string::npos)
            return true;
    }

    return false;
}

std::vector<uint8_t> ClothBaker::build(const RawMesh& raw_mesh) {
    if (raw_mesh.vertices.empty() || raw_mesh.indices.empty())
        return {};

    support::log_info("[ClothBaker] Starting dual-mesh baking for: " + raw_mesh.source_path);
    support::log_info("[ClothBaker] Fine RenderMesh vertices: " + std::to_string(raw_mesh.vertices.size()) +
                      ", indices: " + std::to_string(raw_mesh.indices.size()));

    // 1. Weld coincident 3D points to build single-manifold Coarse SimMesh Base (LOD0)
    std::unordered_map<GridKey, uint32_t, GridKeyHash> grid_map;
    std::vector<bud::math::vec3> sim_positions_lod0;
    std::vector<uint32_t> render_to_sim0(raw_mesh.vertices.size());

    for (size_t i = 0; i < raw_mesh.vertices.size(); ++i) {
        bud::math::vec3 pos(raw_mesh.vertices[i].position[0],
                            raw_mesh.vertices[i].position[1],
                            raw_mesh.vertices[i].position[2]);

        GridKey key{
            static_cast<int64_t>(std::floor(pos.x / kPositionWeldGridCell)),
            static_cast<int64_t>(std::floor(pos.y / kPositionWeldGridCell)),
            static_cast<int64_t>(std::floor(pos.z / kPositionWeldGridCell))
        };

        auto it = grid_map.find(key);
        if (it != grid_map.end()) {
            render_to_sim0[i] = it->second;
        } else {
            uint32_t new_idx = static_cast<uint32_t>(sim_positions_lod0.size());
            sim_positions_lod0.push_back(pos);
            grid_map[key] = new_idx;
            render_to_sim0[i] = new_idx;
        }
    }

    std::vector<uint32_t> sim_indices_lod0;
    sim_indices_lod0.reserve(raw_mesh.indices.size());
    for (size_t i = 0; i + 2 < raw_mesh.indices.size(); i += 3) {
        uint32_t i0 = render_to_sim0[raw_mesh.indices[i + 0]];
        uint32_t i1 = render_to_sim0[raw_mesh.indices[i + 1]];
        uint32_t i2 = render_to_sim0[raw_mesh.indices[i + 2]];
        if (i0 != i1 && i1 != i2 && i2 != i0) {
            sim_indices_lod0.push_back(i0);
            sim_indices_lod0.push_back(i1);
            sim_indices_lod0.push_back(i2);
        }
    }

    support::log_info("[ClothBaker] SimMesh LOD0 welded: " + std::to_string(sim_positions_lod0.size()) +
                      " particles, " + std::to_string(sim_indices_lod0.size() / 3) + " triangles.");

    GeneratedSimMesh sim_mesh_lod0 = build_sim_mesh(sim_positions_lod0, sim_indices_lod0);
    std::vector<bud::physics::ClothSkinBinding> bindings_lod0 = compute_barycentric_bindings(raw_mesh.vertices, sim_mesh_lod0);

    // 2. Generate SimMesh LOD1 using meshoptimizer
    std::vector<uint32_t> simplified_indices(sim_indices_lod0.size());
    size_t target_indices = static_cast<size_t>(sim_indices_lod0.size() * kLod1DecimateRatio);
    size_t result_indices = meshopt_simplify(
        simplified_indices.data(),
        sim_indices_lod0.data(),
        sim_indices_lod0.size(),
        &sim_positions_lod0[0].x,
        sim_positions_lod0.size(),
        sizeof(bud::math::vec3),
        target_indices,
        kLod1MaxError,
        0,
        nullptr
    );
    simplified_indices.resize(result_indices);

    // Compact LOD1 vertices
    std::vector<uint32_t> lod0_to_lod1(sim_positions_lod0.size(), UINT32_MAX);
    std::vector<bud::math::vec3> sim_positions_lod1;
    std::vector<uint32_t> sim_indices_lod1(simplified_indices.size());

    for (size_t i = 0; i < simplified_indices.size(); ++i) {
        uint32_t old_v = simplified_indices[i];
        if (lod0_to_lod1[old_v] == UINT32_MAX) {
            uint32_t new_v = static_cast<uint32_t>(sim_positions_lod1.size());
            lod0_to_lod1[old_v] = new_v;
            sim_positions_lod1.push_back(sim_positions_lod0[old_v]);
        }
        sim_indices_lod1[i] = lod0_to_lod1[old_v];
    }

    if (sim_indices_lod1.empty()) {
        sim_positions_lod1 = sim_positions_lod0;
        sim_indices_lod1 = sim_indices_lod0;
    }

    support::log_info("[ClothBaker] SimMesh LOD1 simplified: " + std::to_string(sim_positions_lod1.size()) +
                      " particles, " + std::to_string(sim_indices_lod1.size() / 3) + " triangles.");

    GeneratedSimMesh sim_mesh_lod1 = build_sim_mesh(sim_positions_lod1, sim_indices_lod1);
    std::vector<bud::physics::ClothSkinBinding> bindings_lod1 = compute_barycentric_bindings(raw_mesh.vertices, sim_mesh_lod1);

    // 3. Serialize to ClothPhysics chunk
    bud::asset::ClothPhysicsChunkHeader chunk_header{};
    chunk_header.magic = bud::asset::CLOTH_PHYSICS_MAGIC;
    chunk_header.version = bud::asset::CLOTH_PHYSICS_VERSION;
    chunk_header.lod_count = 2;
    chunk_header.lod_switch_distances[0] = 8.0f;
    chunk_header.lod_switch_distances[1] = 22.0f;
    chunk_header.lod_switch_distances[2] = 0.0f;
    chunk_header.lod_switch_distances[3] = 0.0f;

    std::vector<uint8_t> payload;
    payload.resize(sizeof(bud::asset::ClothPhysicsChunkHeader)); // reserve header space

    auto append_lod = [&](size_t lod_idx, const GeneratedSimMesh& sm, const std::vector<bud::physics::ClothSkinBinding>& b) {
        bud::asset::ClothPhysicsLODHeader& lh = chunk_header.lods[lod_idx];
        lh.sim_particle_count = static_cast<uint32_t>(sm.particles.size());
        lh.sim_particle_offset = static_cast<uint32_t>(payload.size());
        size_t p_bytes = sm.particles.size() * sizeof(bud::physics::SimParticle);
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(sm.particles.data()), reinterpret_cast<const uint8_t*>(sm.particles.data()) + p_bytes);

        lh.constraint_count = static_cast<uint32_t>(sm.constraints.size());
        lh.constraint_offset = static_cast<uint32_t>(payload.size());
        size_t c_bytes = sm.constraints.size() * sizeof(bud::physics::DistanceConstraint);
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(sm.constraints.data()), reinterpret_cast<const uint8_t*>(sm.constraints.data()) + c_bytes);

        lh.binding_count = static_cast<uint32_t>(b.size());
        lh.binding_offset = static_cast<uint32_t>(payload.size());
        size_t b_bytes = b.size() * sizeof(bud::physics::ClothSkinBinding);
        payload.insert(payload.end(), reinterpret_cast<const uint8_t*>(b.data()), reinterpret_cast<const uint8_t*>(b.data()) + b_bytes);
    };

    append_lod(0, sim_mesh_lod0, bindings_lod0);
    append_lod(1, sim_mesh_lod1, bindings_lod1);

    // Overwrite header at the beginning of payload
    std::memcpy(payload.data(), &chunk_header, sizeof(bud::asset::ClothPhysicsChunkHeader));

    support::log_info("[ClothBaker] Successfully generated ClothPhysics chunk, total payload: " + std::to_string(payload.size()) + " bytes.");
    return payload;
}

} // namespace bud::asset_pipeline
