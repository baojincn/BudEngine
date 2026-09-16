#include "src/robots/bud.robot.visual_bridge.hpp"
#include "src/runtime/bud.engine.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"
#include "src/io/bud.io.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bud::robots {

namespace {

// Angle-aware normal smoothing with crease preservation for CAD/STL meshes
static void smooth_mesh_normals(bud::io::MeshData& mesh, float max_smoothing_angle_deg = 75.0f) {
    if (mesh.vertices.empty() || mesh.indices.size() < 3)
        return;

    const size_t tri_count = mesh.indices.size() / 3;
    const float cos_crease_threshold = std::cos(glm::radians(max_smoothing_angle_deg));

    struct FaceNormalData {
        glm::vec3 normal;
        float area_weight;
    };

    std::vector<FaceNormalData> face_normals(tri_count);

    // Spatial clustering key for vertex positions (welds duplicated STL vertices)
    struct PosKey {
        int64_t x, y, z;
        bool operator==(const PosKey& o) const {
            return x == o.x && y == o.y && z == o.z;
        }
    };
    struct PosKeyHash {
        size_t operator()(const PosKey& k) const {
            size_t h1 = std::hash<int64_t>()(k.x);
            size_t h2 = std::hash<int64_t>()(k.y);
            size_t h3 = std::hash<int64_t>()(k.z);
            return h1 ^ (h2 << 1) ^ (h3 << 2);
        }
    };

    auto to_pos_key = [](const glm::vec3& p) {
        constexpr float quantization_scale = 10000.0f; // 0.1 mm precision
        return PosKey{
            static_cast<int64_t>(std::round(p.x * quantization_scale)),
            static_cast<int64_t>(std::round(p.y * quantization_scale)),
            static_cast<int64_t>(std::round(p.z * quantization_scale))
        };
    };

    // Calculate per-face normals and area weights
    for (size_t t = 0; t < tri_count; ++t) {
        uint32_t i0 = mesh.indices[t * 3 + 0];
        uint32_t i1 = mesh.indices[t * 3 + 1];
        uint32_t i2 = mesh.indices[t * 3 + 2];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) {
            face_normals[t] = { glm::vec3(0.0f, 1.0f, 0.0f), 0.0f };
            continue;
        }

        const glm::vec3& p0 = mesh.vertices[i0].pos;
        const glm::vec3& p1 = mesh.vertices[i1].pos;
        const glm::vec3& p2 = mesh.vertices[i2].pos;

        glm::vec3 cross_prod = glm::cross(p1 - p0, p2 - p0);
        float len = glm::length(cross_prod);
        if (len > 1e-8f)
            face_normals[t] = { cross_prod / len, len };
        else
            face_normals[t] = { glm::vec3(0.0f, 1.0f, 0.0f), 0.0f };
    }

    // Map each unique spatial position to all (triangle_index, vertex_index) that touch it
    struct VertexRef {
        uint32_t tri_idx;
        uint32_t vert_idx;
    };
    std::unordered_map<PosKey, std::vector<VertexRef>, PosKeyHash> clusters;
    clusters.reserve(mesh.vertices.size() / 2);

    for (size_t t = 0; t < tri_count; ++t) {
        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t v_idx = mesh.indices[t * 3 + c];
            if (v_idx < mesh.vertices.size())
                clusters[to_pos_key(mesh.vertices[v_idx].pos)].push_back({ static_cast<uint32_t>(t), v_idx });
        }
    }

    // For each cluster, smooth normals across faces within the dihedral crease angle threshold
    for (const auto& [key, refs] : clusters) {
        for (const auto& current_ref : refs) {
            const auto& current_face = face_normals[current_ref.tri_idx];
            glm::vec3 accum_normal(0.0f);

            for (const auto& other_ref : refs) {
                const auto& other_face = face_normals[other_ref.tri_idx];
                float dot_val = glm::dot(current_face.normal, other_face.normal);
                if (dot_val >= cos_crease_threshold)
                    accum_normal += other_face.normal * other_face.area_weight;
            }

            float accum_len2 = glm::dot(accum_normal, accum_normal);
            if (accum_len2 > 1e-8f)
                mesh.vertices[current_ref.vert_idx].normal = accum_normal * glm::inversesqrt(accum_len2);
            else
                mesh.vertices[current_ref.vert_idx].normal = current_face.normal;
        }
    }
}


bud::math::mat4 calc_visual_local_transform(const bud::robots::VisualDef& vis) {
    bud::math::mat4 t = glm::translate(bud::math::mat4(1.0f),
        bud::math::vec3(vis.origin_xyz[0], vis.origin_xyz[1], vis.origin_xyz[2]));

    float roll = vis.origin_rpy[0];
    float pitch = vis.origin_rpy[1];
    float yaw = vis.origin_rpy[2];

    glm::quat q = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f))
                * glm::angleAxis(pitch, glm::vec3(0.0f, 1.0f, 0.0f))
                * glm::angleAxis(roll, glm::vec3(1.0f, 0.0f, 0.0f));

    bud::math::mat4 r = glm::mat4_cast(q);

    float sx = vis.geometry.scale[0] != 0.0f ? vis.geometry.scale[0] : 1.0f;
    float sy_val = vis.geometry.scale[1] != 0.0f ? vis.geometry.scale[1] : 1.0f;
    float sz = vis.geometry.scale[2] != 0.0f ? vis.geometry.scale[2] : 1.0f;
    bud::math::mat4 s = glm::scale(bud::math::mat4(1.0f), bud::math::vec3(sx, sy_val, sz));

    return t * r * s;
}

} // namespace

bool RobotVisualBridge::init(bud::engine::BudEngine* engine,
                             bud::scene::Scene& scene,
                             const bud::robots::RobotDef& robot_def,
                             const std::string& package_root) {
    if (!engine)
        return false;

    auto* asset_manager = engine->get_asset_manager();
    auto* renderer = engine->get_renderer();
    if (!asset_manager || !renderer)
        return false;

    m_parts.clear();
    m_link_to_part_indices.clear();

    std::unordered_map<std::string, std::vector<size_t>> path_to_entity_indices;

    for (const auto& link : robot_def.links) {
        for (size_t v_idx = 0; v_idx < link.visuals.size(); ++v_idx) {
            const auto& vis = link.visuals[v_idx];
            if (vis.geometry.mesh_path.empty())
                continue;

            std::string disk_asset_path = package_root;
            if (!disk_asset_path.empty() && disk_asset_path.back() != '/' && disk_asset_path.back() != '\\')
                disk_asset_path += "/";
            disk_asset_path += vis.geometry.mesh_path;

            std::string entity_name = "robot_" + robot_def.name + "_" + link.name;
            if (link.visuals.size() > 1)
                entity_name += "_" + std::to_string(v_idx);

            size_t ent_idx = scene.entities.size();
            for (size_t i = 0; i < scene.entities.size(); ++i) {
                if (scene.entities[i].name == entity_name) {
                    ent_idx = i;
                    break;
                }
            }

            if (ent_idx == scene.entities.size()) {
                bud::scene::Entity entity{};
                entity.name = entity_name;
                entity.asset_path = disk_asset_path;
                entity.is_static = false; // Must be false for dynamic GPU scene instances
                entity.is_active = true;
                entity.is_cast_shadow = true;
                entity.is_receive_shadow = true;
                entity.enable_physics = false; // Rigid bodies managed by Jolt Ragdoll
                entity.transform = bud::math::mat4(1.0f);
                entity.root_group_index = bud::asset::INVALID_INDEX;
                entity.base_virtual_page = bud::asset::INVALID_INDEX;

                scene.entities.push_back(entity);
            }

            VisualPartEntry entry{};
            entry.link_name = link.name;
            entry.entity_index = ent_idx;
            entry.local_offset = calc_visual_local_transform(vis);

            size_t part_idx = m_parts.size();
            m_parts.push_back(entry);
            m_link_to_part_indices[link.name].push_back(part_idx);

            path_to_entity_indices[disk_asset_path].push_back(ent_idx);
        }
    }

    std::cout << "[RobotVisualBridge] Registered " << m_parts.size() 
              << " visual entities for robot '" << robot_def.name << "'." << std::endl;

    auto total_mesh_count = std::make_shared<std::atomic<uint32_t>>(0);
    const uint32_t expected_meshes = static_cast<uint32_t>(path_to_entity_indices.size());

    // Asynchronously load distinct visual meshes and upload to GPU
    for (const auto& [path, indices] : path_to_entity_indices) {
        asset_manager->load_mesh_async(path, [engine, indices, path, total_mesh_count, expected_meshes](bud::io::MeshData mesh) {
            auto* cur_renderer = engine->get_renderer();
            if (!cur_renderer)
                return;

            // Recompute smooth normals for CAD/STL meshes to eliminate flat-faceted shading
            smooth_mesh_normals(mesh, 75.0f);

            // Clear placeholder default.png checkerboard textures so the robot renders with realistic PBR materials
            mesh.texture_paths.clear();
            mesh.materials.clear();

            bud::io::MeshData::Material mat{};
            std::string lower_path = path;
            std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);

            if (lower_path.find("logo") != std::string::npos) {
                // Unitree 3D logo: high-gloss obsidian black lettering on chest
                mat.base_color_factor = glm::vec4(0.015f, 0.015f, 0.02f, 1.0f);
                mat.metallic_factor = 0.90f;
                mat.roughness_factor = 0.08f;
            }
            else if (lower_path.find("head") != std::string::npos) {
                // Head visor: high-gloss deep tinted optical glass / dark visor
                mat.base_color_factor = glm::vec4(0.02f, 0.02f, 0.025f, 1.0f);
                mat.metallic_factor = 0.92f;
                mat.roughness_factor = 0.06f;
            }
            else if (lower_path.find("rubber") != std::string::npos || lower_path.find("ankle_roll") != std::string::npos) {
                // Rubber hands / foot soles: matte vulcanized rubber
                mat.base_color_factor = glm::vec4(0.08f, 0.08f, 0.09f, 1.0f);
                mat.metallic_factor = 0.02f;
                mat.roughness_factor = 0.85f;
            }
            else if (lower_path.find("pelvis_contour") == std::string::npos &&
                     (lower_path.find("pelvis") != std::string::npos || lower_path.find("hip_pitch") != std::string::npos)) {
                // Pelvis internal chassis & hip pitch joints: machined gunmetal titanium
                mat.base_color_factor = glm::vec4(0.32f, 0.33f, 0.35f, 1.0f);
                mat.metallic_factor = 0.95f;
                mat.roughness_factor = 0.22f;
            }
            else if (lower_path.find("knee") != std::string::npos || lower_path.find("elbow") != std::string::npos ||
                     lower_path.find("pitch") != std::string::npos || lower_path.find("roll") != std::string::npos ||
                     lower_path.find("yaw") != std::string::npos || lower_path.find("wrist") != std::string::npos) {
                // Joint actuators & limb structures: CNC machined aerospace titanium alloy
                mat.base_color_factor = glm::vec4(0.52f, 0.54f, 0.57f, 1.0f);
                mat.metallic_factor = 0.96f;
                mat.roughness_factor = 0.20f;
            }
            else {
                // Torso / Pelvis contour / Armor plates: Unitree pearl metallic silver
                mat.base_color_factor = glm::vec4(0.88f, 0.88f, 0.90f, 1.0f);
                mat.metallic_factor = 0.88f;
                mat.roughness_factor = 0.22f;
            }

            mesh.materials.push_back(mat);
            for (auto& s : mesh.subsets) {
                s.material_index = 0;
            }

            auto handle = cur_renderer->upload_mesh(mesh);
            if (!handle.is_valid())
                return;

            // Aligned with cloth pipeline: expand bounds to prevent aggressive frustum culling on dynamic parts
            bud::math::AABB bounds{};
            for (const auto& v : mesh.vertices) {
                bounds.merge(bud::math::vec3(v.pos[0], v.pos[1], v.pos[2]));
            }
            bounds.min -= bud::math::vec3(1.0f);
            bounds.max += bud::math::vec3(1.0f);
            cur_renderer->update_mesh_bounds(handle.mesh_id, bounds);

            auto& cur_scene = engine->get_scene();
            for (size_t e_idx : indices) {
                if (e_idx < cur_scene.entities.size()) {
                    cur_scene.entities[e_idx].mesh_index = handle.mesh_id;
                    cur_scene.entities[e_idx].material_index = handle.material_id;
                }
            }

            uint32_t loaded = total_mesh_count->fetch_add(1, std::memory_order_relaxed) + 1;
            std::cout << "[RobotVisualBridge] Uploaded mesh (" << loaded << "/" << expected_meshes 
                      << "): " << path << " (mesh_id=" << handle.mesh_id << ")" << std::endl;
        });
    }

    m_initialized = true;
    m_visible = true;
    return true;
}

void RobotVisualBridge::sync_transforms(const bud::robots::RobotInstance& robot,
                                        bud::scene::Scene& scene) {
    if (!m_initialized || !m_visible)
        return;

    auto transforms = robot.get_all_link_transforms();

    // In MuJoCo simulation mode the link world positions/rotations returned by
    // get_all_link_transforms() have been converted from MuJoCo Z-up to engine Y-up via
    // from_mujoco().  The visual local offsets (origin_xyz/origin_rpy from the URDF) are
    // still in URDF/MuJoCo Z-up space.  We must express local_offset in engine Y-up space
    // before composing it with the (already engine-space) world link matrix.
    //
    // Basis change B from URDF Z-up to engine Y-up: from_mujoco(v) = (v.x, v.z, -v.y).
    // As a 4x4 column-major matrix (GLM convention):
    //   col0 = (1, 0, 0, 0)   - X unchanged
    //   col1 = (0, 0,-1, 0)   - URDF Y  →  -engine Z
    //   col2 = (0, 1, 0, 0)   - URDF Z  →   engine Y
    //   col3 = (0, 0, 0, 1)
    // local_offset_engine = B * local_offset_urdf * B^{-1}
    // Since B is orthonormal, B^{-1} = B^T.
    static const bud::math::mat4 k_B(
        bud::math::vec4(1.f, 0.f,  0.f, 0.f),
        bud::math::vec4(0.f, 0.f, -1.f, 0.f),
        bud::math::vec4(0.f, 1.f,  0.f, 0.f),
        bud::math::vec4(0.f, 0.f,  0.f, 1.f));
    static const bud::math::mat4 k_Binv = glm::transpose(bud::math::mat4(
        bud::math::vec4(1.f, 0.f,  0.f, 0.f),
        bud::math::vec4(0.f, 0.f, -1.f, 0.f),
        bud::math::vec4(0.f, 1.f,  0.f, 0.f),
        bud::math::vec4(0.f, 0.f,  0.f, 1.f)));

    for (const auto& lt : transforms) {
        auto it = m_link_to_part_indices.find(lt.link_name);
        if (it == m_link_to_part_indices.end())
            continue;

        bud::math::mat4 world_link_mat = 
            glm::translate(bud::math::mat4(1.0f), lt.position) * 
            glm::mat4_cast(lt.rotation);

        for (size_t p_idx : it->second) {
            if (p_idx >= m_parts.size())
                continue;

            const auto& part = m_parts[p_idx];
            if (part.entity_index < scene.entities.size()) {
                auto& ent = scene.entities[part.entity_index];

                bud::math::mat4 final_offset = part.local_offset;
                if (m_is_simulation)
                    final_offset = k_B * part.local_offset * k_Binv;

                if (!ent.has_prev_transform) {
                    ent.prev_transform = world_link_mat * final_offset;
                    ent.has_prev_transform = true;
                } else {
                    ent.prev_transform = ent.transform;
                }
                ent.transform = world_link_mat * final_offset;
            }
        }
    }
}

void RobotVisualBridge::sync_transforms(const std::unordered_map<std::string, glm::mat4>& world_link_transforms,
                                        bud::scene::Scene& scene) {
    if (!m_initialized || !m_visible)
        return;

    for (const auto& [name, world_link_mat] : world_link_transforms) {
        auto it = m_link_to_part_indices.find(name);
        if (it == m_link_to_part_indices.end())
            continue;

        for (size_t p_idx : it->second) {
            if (p_idx >= m_parts.size())
                continue;

            const auto& part = m_parts[p_idx];
            if (part.entity_index < scene.entities.size()) {
                auto& ent = scene.entities[part.entity_index];
                if (!ent.has_prev_transform) {
                    ent.prev_transform = world_link_mat * part.local_offset;
                    ent.has_prev_transform = true;
                } else {
                    ent.prev_transform = ent.transform;
                }
                ent.transform = world_link_mat * part.local_offset;
            }
        }
    }
}

void RobotVisualBridge::set_visible(bool visible, bud::scene::Scene& scene) {
    m_visible = visible;
    for (const auto& part : m_parts) {
        if (part.entity_index < scene.entities.size()) {
            scene.entities[part.entity_index].is_active = visible;
            if (!visible)
                scene.entities[part.entity_index].transform = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
        }
    }
}

} // namespace bud::robots
