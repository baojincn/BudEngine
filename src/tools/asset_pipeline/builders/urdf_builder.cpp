#include "urdf_builder.hpp"
#include "../importers/urdf_parser.hpp"
#include "../importers/stl_importer.hpp"
#include "../importers/dae_importer.hpp"
#include "../importers/obj_importer.hpp"
#include "../importers/gltf_importer.hpp"
#include "../core/serializer.hpp"
#include "../core/support.hpp"
#include "../cache/asset_registry.hpp"

#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>

namespace fs = std::filesystem;

namespace bud::asset_pipeline {

namespace {

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

std::string to_lower_ext(const std::string& path_str) {
    std::string ext = fs::path(path_str).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::optional<bud::asset::RawMesh> import_mesh_any(const std::string& filepath, float scale) {
    std::string ext = to_lower_ext(filepath);
    if (ext == ".stl")
        return StlImporter::import_from_file(filepath, scale);
    if (ext == ".dae")
        return DaeImporter::import_from_file(filepath, scale);
    if (ext == ".obj")
        return ObjImporter::import_from_file(filepath, scale);
    if (ext == ".gltf" || ext == ".glb")
        return GltfImporter::import_from_file(filepath);
    return std::nullopt;
}

std::optional<bud::asset::RawMesh> load_raw_mesh_from_budasset_or_source(const std::string& filepath, float scale) {
    if (fs::exists(filepath)) {
        std::string ext = to_lower_ext(filepath);
        if (ext == ".budasset") {
            std::ifstream in(filepath, std::ios::binary | std::ios::ate);
            if (in.is_open()) {
                size_t sz = static_cast<size_t>(in.tellg());
                in.seekg(0);
                std::vector<uint8_t> buf(sz);
                in.read(reinterpret_cast<char*>(buf.data()), sz);

                auto reader = BudAssetReader::load_from_memory(buf.data(), buf.size());
                if (reader) {
                    auto chunk = reader->get_chunk_data(AssetChunkType::RawMesh);
                    if (chunk)
                        return bud::asset::RawMesh::deserialize_binary(chunk->data(), chunk->size());
                }
            }
        }
    }
    return import_mesh_any(filepath, scale);
}

std::vector<uint8_t> serialize_convex_hull(const bud::robot::ConvexHullData& hull) {
    std::vector<uint8_t> buffer;
    auto write_bytes = [&](const void* ptr, size_t sz) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ptr);
        buffer.insert(buffer.end(), p, p + sz);
    };

    uint32_t pt_count = static_cast<uint32_t>(hull.points.size());
    write_bytes(&pt_count, sizeof(pt_count));
    if (pt_count > 0)
        write_bytes(hull.points.data(), pt_count * sizeof(float));

    uint32_t idx_count = static_cast<uint32_t>(hull.indices.size());
    write_bytes(&idx_count, sizeof(idx_count));
    if (idx_count > 0)
        write_bytes(hull.indices.data(), idx_count * sizeof(uint32_t));

    write_bytes(hull.aabb_min, sizeof(float) * 3);
    write_bytes(hull.aabb_max, sizeof(float) * 3);
    write_bytes(hull.center_of_mass, sizeof(float) * 3);
    write_bytes(&hull.volume, sizeof(float));
    return buffer;
}

std::optional<bud::robot::ConvexHullData> extract_convex_hull_from_raw_mesh(const bud::asset::RawMesh& mesh, int max_vertices) {
    ensure_jolt_initialized();
    if (mesh.vertices.empty())
        return std::nullopt;

    JPH::Array<JPH::Vec3> in_positions;
    in_positions.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
        in_positions.push_back(JPH::Vec3(v.position[0], v.position[1], v.position[2]));
    }

    bud::robot::ConvexHullData hull_data;
    hull_data.aabb_min[0] = hull_data.aabb_min[1] = hull_data.aabb_min[2] = 1e30f;
    hull_data.aabb_max[0] = hull_data.aabb_max[1] = hull_data.aabb_max[2] = -1e30f;

    const char* error_msg = nullptr;
    constexpr float hull_tolerance = 1.0e-3f;
    JPH::ConvexHullBuilder builder(in_positions);
    JPH::ConvexHullBuilder::EResult result = builder.Initialize(max_vertices, hull_tolerance, error_msg);

    if (result == JPH::ConvexHullBuilder::EResult::Success ||
        result == JPH::ConvexHullBuilder::EResult::MaxVerticesReached) {

        JPH::Vec3 com{ 0.0f, 0.0f, 0.0f };
        float volume = 0.0f;
        builder.GetCenterOfMassAndVolume(com, volume);

        hull_data.center_of_mass[0] = com.GetX();
        hull_data.center_of_mass[1] = com.GetY();
        hull_data.center_of_mass[2] = com.GetZ();
        hull_data.volume = volume;

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
                    uint32_t new_idx = static_cast<uint32_t>(hull_data.points.size() / 3);
                    const JPH::Vec3& p = in_positions[orig_idx];
                    hull_data.points.push_back(p.GetX());
                    hull_data.points.push_back(p.GetY());
                    hull_data.points.push_back(p.GetZ());

                    hull_data.aabb_min[0] = std::min(hull_data.aabb_min[0], p.GetX());
                    hull_data.aabb_min[1] = std::min(hull_data.aabb_min[1], p.GetY());
                    hull_data.aabb_min[2] = std::min(hull_data.aabb_min[2], p.GetZ());

                    hull_data.aabb_max[0] = std::max(hull_data.aabb_max[0], p.GetX());
                    hull_data.aabb_max[1] = std::max(hull_data.aabb_max[1], p.GetY());
                    hull_data.aabb_max[2] = std::max(hull_data.aabb_max[2], p.GetZ());

                    global_to_local[orig_idx] = new_idx;
                    local_poly.push_back(new_idx);
                }
                else {
                    local_poly.push_back(it->second);
                }
            }

            for (size_t i = 1; i + 1 < local_poly.size(); ++i) {
                hull_data.indices.push_back(local_poly[0]);
                hull_data.indices.push_back(local_poly[i]);
                hull_data.indices.push_back(local_poly[i + 1]);
            }
        }
        return hull_data;
    }

    return std::nullopt;
}

} // namespace

bool UrdfBuilder::cook_urdf(const std::string& urdf_path, const std::string& output_robot_dir, const UrdfBuildOptions& options) {
    ensure_jolt_initialized();
    support::log_info("[UrdfBuilder] Starting URDF cooking: " + urdf_path + " -> " + output_robot_dir);

    UrdfParseOptions parse_opts{};
    parse_opts.package_root = options.package_root;
    parse_opts.auto_detect_root = true;

    auto robot_def_opt = UrdfParser::parse_file(urdf_path, parse_opts);
    if (!robot_def_opt) {
        support::log_error("[UrdfBuilder] Failed to parse URDF file: " + urdf_path);
        return false;
    }

    return cook_robot(std::move(*robot_def_opt), output_robot_dir, options);
}

bool UrdfBuilder::cook_robot(bud::robot::RobotDef robot_def, const std::string& output_robot_dir, const UrdfBuildOptions& options) {
    ensure_jolt_initialized();
    auto t_start = std::chrono::steady_clock::now();
    support::log_info("[UrdfBuilder] Starting RobotDef cooking: " + robot_def.name + " -> " + output_robot_dir);

    fs::path out_root(output_robot_dir);
    fs::path vis_dir = out_root / "meshes" / "visual";
    fs::path col_dir = out_root / "meshes" / "collision";

    std::error_code ec;
    fs::create_directories(vis_dir, ec);
    fs::create_directories(col_dir, ec);

    uint32_t visual_cooked_count = 0;
    uint32_t visual_cached_count = 0;
    uint32_t collision_cooked_count = 0;
    uint32_t collision_cached_count = 0;

    // 1. Process Visual Meshes
    for (auto& link : robot_def.links) {
        for (auto& vis : link.visuals) {
            if (vis.geometry.type != bud::robot::GeometryType::Mesh)
                continue;

            std::string src_path = vis.geometry.mesh_path;
            if (src_path.empty())
                continue;

            fs::path src_p(src_path);
            std::string stem = src_p.stem().string();
            std::string rel_asset_path = "meshes/visual/" + stem + ".budasset";
            fs::path disk_asset_path = out_root / rel_asset_path;

            bool need_cook = true;
            if (options.use_cache && fs::exists(disk_asset_path)) {
                need_cook = false;
                visual_cached_count++;
            }

            if (need_cook) {
                float effective_scale = options.scale;
                if (effective_scale <= 0.0f && vis.geometry.scale[0] > 0.0f && vis.geometry.scale[0] != 1.0f)
                    effective_scale = vis.geometry.scale[0];

                auto raw_mesh_opt = import_mesh_any(src_path, effective_scale);
                if (raw_mesh_opt) {
                    uint64_t asset_id = compute_asset_id(rel_asset_path);
                    BudAssetWriter writer(AssetType::Mesh, asset_id);

                    std::vector<uint8_t> raw_data = raw_mesh_opt->serialize_binary();
                    writer.add_chunk(AssetChunkType::RawMesh, raw_data.data(), raw_data.size());

                    if (writer.save_to_file(disk_asset_path.string())) {
                        visual_cooked_count++;
                    } else {
                        support::log_warn("[UrdfBuilder] Failed to write visual .budasset: " + disk_asset_path.string());
                    }
                } else {
                    support::log_warn("[UrdfBuilder] Failed to import visual mesh: " + src_path);
                }
            }

            // Update URDF link visual geometry path to mirrored relative asset path
            vis.geometry.mesh_path = rel_asset_path;
        }
    }

    // Synthesize mesh collision for links that have visual meshes but no mesh collision (rubber hands, foot soles)
    for (auto& link : robot_def.links) {
        bool has_mesh_col = false;
        for (const auto& col : link.collisions) {
            if (col.geometry.type == bud::robot::GeometryType::Mesh) {
                has_mesh_col = true;
                break;
            }
        }
        if (!has_mesh_col && !link.visuals.empty()) {
            const auto& vis = link.visuals.front();
            if (vis.geometry.type == bud::robot::GeometryType::Mesh && !vis.geometry.mesh_path.empty()) {
                bud::robot::CollisionDef col_def;
                col_def.name = link.name + "_collision";
                for (int i = 0; i < 3; ++i) {
                    col_def.origin_xyz[i] = vis.origin_xyz[i];
                    col_def.origin_rpy[i] = vis.origin_rpy[i];
                    col_def.geometry.scale[i] = vis.geometry.scale[i];
                }
                col_def.geometry.type = bud::robot::GeometryType::Mesh;
                col_def.geometry.mesh_path = vis.geometry.mesh_path;

                link.collisions.clear();
                link.collisions.push_back(std::move(col_def));
            }
        }
    }

    // 2. Process Collision Meshes & Convex Hull Generation (Isaac Sim style)
    for (auto& link : robot_def.links) {
        for (auto& col : link.collisions) {
            if (col.geometry.type == bud::robot::GeometryType::Mesh) {
                std::string src_path = col.geometry.mesh_path;
                if (src_path.empty())
                    continue;

                fs::path src_p(src_path);
                std::string stem = src_p.stem().string();

                float effective_scale = options.scale;
                if (effective_scale <= 0.0f && col.geometry.scale[0] > 0.0f && col.geometry.scale[0] != 1.0f)
                    effective_scale = col.geometry.scale[0];

                // Offline Convex Hull Generation (Isaac Sim style convex polygon generation via Jolt)
                if (!col.convex_hull.points.empty()) {
                    collision_cooked_count++;
                } else if (fs::exists(src_path) && to_lower_ext(src_path) != ".budasset") {
                    auto hull_opt = StlImporter::extract_collision_geometry(src_path, effective_scale, options.max_convex_vertices);
                    if (hull_opt) {
                        col.convex_hull = std::move(*hull_opt);
                        collision_cooked_count++;
                    } else {
                        support::log_warn("[UrdfBuilder] Convex hull generation failed for collision mesh: " + src_path);
                    }
                } else {
                    std::string disk_p = fs::exists(src_path) ? src_path : (out_root / src_path).string();
                    auto raw_mesh_opt = load_raw_mesh_from_budasset_or_source(disk_p, effective_scale);
                    if (raw_mesh_opt) {
                        auto hull_opt = extract_convex_hull_from_raw_mesh(*raw_mesh_opt, options.max_convex_vertices);
                        if (hull_opt) {
                            col.convex_hull = std::move(*hull_opt);
                            collision_cooked_count++;
                        }
                    }
                }

                // Check if identical visual mesh exists (Deduplication: reuse visual mesh reference)
                std::string vis_rel_asset = "meshes/visual/" + stem + ".budasset";
                if (fs::exists(out_root / vis_rel_asset)) {
                    col.geometry.mesh_path = vis_rel_asset;
                } else {
                    std::string rel_asset_path = "meshes/collision/" + stem + ".budasset";
                    fs::path disk_asset_path = out_root / rel_asset_path;

                    bool need_cook = true;
                    if (options.use_cache && fs::exists(disk_asset_path)) {
                        need_cook = false;
                        collision_cached_count++;
                    }

                    if (need_cook) {
                        uint64_t asset_id = compute_asset_id(rel_asset_path);
                        BudAssetWriter writer(AssetType::Physics, asset_id);

                        std::vector<uint8_t> hull_bytes = serialize_convex_hull(col.convex_hull);
                        writer.add_chunk(AssetChunkType::Collision, hull_bytes.data(), hull_bytes.size());

                        auto raw_mesh_opt = import_mesh_any(src_path, effective_scale);
                        if (raw_mesh_opt) {
                            std::vector<uint8_t> raw_data = raw_mesh_opt->serialize_binary();
                            writer.add_chunk(AssetChunkType::RawMesh, raw_data.data(), raw_data.size());
                        }

                        if (!writer.save_to_file(disk_asset_path.string()))
                            support::log_warn("[UrdfBuilder] Failed to write collision .budasset: " + disk_asset_path.string());
                    }
                    col.geometry.mesh_path = rel_asset_path;
                }
            } else if (col.geometry.type == bud::robot::GeometryType::Box) {
                // Generate analytical box convex hull
                float hx = col.geometry.box_size[0] * 0.5f;
                float hy = col.geometry.box_size[1] * 0.5f;
                float hz = col.geometry.box_size[2] * 0.5f;
                col.convex_hull.points = {
                    -hx, -hy, -hz,
                     hx, -hy, -hz,
                     hx,  hy, -hz,
                    -hx,  hy, -hz,
                    -hx, -hy,  hz,
                     hx, -hy,  hz,
                     hx,  hy,  hz,
                    -hx,  hy,  hz
                };
                col.convex_hull.aabb_min[0] = -hx;
                col.convex_hull.aabb_min[1] = -hy;
                col.convex_hull.aabb_min[2] = -hz;
                col.convex_hull.aabb_max[0] = hx;
                col.convex_hull.aabb_max[1] = hy;
                col.convex_hull.aabb_max[2] = hz;
                col.convex_hull.volume = col.geometry.box_size[0] * col.geometry.box_size[1] * col.geometry.box_size[2];
            } else if (col.geometry.type == bud::robot::GeometryType::Sphere) {
                float r = col.geometry.sphere_radius;
                col.convex_hull.aabb_min[0] = -r;
                col.convex_hull.aabb_min[1] = -r;
                col.convex_hull.aabb_min[2] = -r;
                col.convex_hull.aabb_max[0] = r;
                col.convex_hull.aabb_max[1] = r;
                col.convex_hull.aabb_max[2] = r;
                col.convex_hull.volume = (4.0f / 3.0f) * 3.14159265f * r * r * r;
            }
        }
    }

    // 3. Save Master Articulation Asset (.budasset Container)
    std::string master_asset_path = (out_root / (robot_def.name + ".budasset")).string();
    uint64_t master_asset_id = compute_asset_id(robot_def.name + ".budasset");
    BudAssetWriter master_writer(AssetType::Articulation, master_asset_id);

    // Build AssetManifest with external references
    std::vector<bud::asset::AssetReference> manifest;
    std::unordered_set<std::string> added_refs;
    for (const auto& link : robot_def.links) {
        for (const auto& vis : link.visuals) {
            if (!vis.geometry.mesh_path.empty() && added_refs.insert(vis.geometry.mesh_path).second) {
                bud::asset::AssetReference ref{};
                ref.asset_id = compute_asset_id(vis.geometry.mesh_path);
                strncpy_s(ref.relative_path, vis.geometry.mesh_path.c_str(), sizeof(ref.relative_path) - 1);
                manifest.push_back(ref);
            }
        }
        for (const auto& col : link.collisions) {
            if (!col.geometry.mesh_path.empty() && added_refs.insert(col.geometry.mesh_path).second) {
                bud::asset::AssetReference ref{};
                ref.asset_id = compute_asset_id(col.geometry.mesh_path);
                strncpy_s(ref.relative_path, col.geometry.mesh_path.c_str(), sizeof(ref.relative_path) - 1);
                manifest.push_back(ref);
            }
        }
    }

    master_writer.add_chunk(
        AssetChunkType::AssetManifest,
        manifest.data(),
        manifest.size() * sizeof(bud::asset::AssetReference),
        static_cast<uint32_t>(AssetChunkFlags::ExternalReference)
    );

    // Add Articulation Chunk with full robot kinematics & dynamics definition
    std::vector<uint8_t> robot_bin = robot_def.serialize_binary();
    master_writer.add_chunk(AssetChunkType::Articulation, robot_bin.data(), robot_bin.size());

    if (!master_writer.save_to_file(master_asset_path)) {
        support::log_error("[UrdfBuilder] Failed to write master .budasset file: " + master_asset_path);
        return false;
    }

    if (options.dump_json) {
        std::string master_json_path = (out_root / (robot_def.name + ".budasset.json")).string();
        robot_def.save_json(master_json_path);
    }

    // Calculate Degrees of Freedom (active joints: Revolute, Prismatic, Continuous)
    uint32_t active_dofs = 0;
    for (const auto& j : robot_def.joints) {
        if (j.type == bud::robot::JointType::Revolute ||
            j.type == bud::robot::JointType::Continuous ||
            j.type == bud::robot::JointType::Prismatic) {
            active_dofs++;
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    std::cout << "==========================================================" << std::endl;
    std::cout << "[UrdfBuilder] Successfully Cooked Robot: " << robot_def.name << std::endl;
    std::cout << "  Root Link:        " << robot_def.root_link << std::endl;
    std::cout << "  Total Links:      " << robot_def.links.size() << std::endl;
    std::cout << "  Total Joints:     " << robot_def.joints.size() << " (Active DoFs: " << active_dofs << ")" << std::endl;
    std::cout << "  Visual Meshes:    " << (visual_cooked_count + visual_cached_count) << " (" << visual_cooked_count << " cooked, " << visual_cached_count << " cached)" << std::endl;
    std::cout << "  Collision Hulls:  " << collision_cooked_count << " (Jolt Convex Hull <= " << options.max_convex_vertices << " vertices)" << std::endl;
    std::cout << "  Master Asset:     " << master_asset_path << " (AssetType::Articulation)" << std::endl;
    std::cout << "  Cook Duration:    " << duration_ms << " ms" << std::endl;
    std::cout << "==========================================================" << std::endl;

    return true;
}

bool UrdfBuilder::cook_robot_to_single_asset(
    const bud::robot::RobotDef& robot_def,
    const std::string& package_root,
    const std::string& output_budasset_path)
{
    if (robot_def.links.empty())
        return false;

    std::string root_link_name = robot_def.root_link;
    if (root_link_name.empty())
        root_link_name = robot_def.links.front().name;

    // 1. Compute link world transforms in robot coordinate system via BFS from root link
    std::unordered_map<std::string, glm::mat4> link_xforms;
    link_xforms[root_link_name] = glm::mat4(1.0f);

    std::queue<std::string> q;
    q.push(root_link_name);
    std::unordered_set<std::string> visited;
    visited.insert(root_link_name);

    while (!q.empty()) {
        std::string parent_name = q.front();
        q.pop();

        glm::mat4 parent_mat = link_xforms[parent_name];
        auto child_joints = robot_def.get_child_joints(parent_name);

        for (const auto* joint : child_joints) {
            if (!joint || joint->child_link.empty())
                continue;
            if (visited.find(joint->child_link) != visited.end())
                continue;

            glm::vec3 j_pos(joint->origin_xyz[0], joint->origin_xyz[1], joint->origin_xyz[2]);
            glm::quat j_rot = glm::angleAxis(joint->origin_rpy[2], glm::vec3(0.0f, 0.0f, 1.0f))
                            * glm::angleAxis(joint->origin_rpy[1], glm::vec3(0.0f, 1.0f, 0.0f))
                            * glm::angleAxis(joint->origin_rpy[0], glm::vec3(1.0f, 0.0f, 0.0f));

            glm::mat4 joint_local = glm::translate(glm::mat4(1.0f), j_pos) * glm::mat4_cast(j_rot);
            glm::mat4 child_mat = parent_mat * joint_local;

            link_xforms[joint->child_link] = child_mat;
            visited.insert(joint->child_link);
            q.push(joint->child_link);
        }
    }

    // 2. Basis conversion from URDF standard (+X fwd, +Y left, +Z up)
    //    to BudEngine standard (+Y up, -Z fwd, +X right)
    glm::mat4 basis_rot(
        glm::vec4( 0.0f,  0.0f, -1.0f, 0.0f),
        glm::vec4(-1.0f,  0.0f,  0.0f, 0.0f),
        glm::vec4( 0.0f,  1.0f,  0.0f, 0.0f),
        glm::vec4( 0.0f,  0.0f,  0.0f, 1.0f)
    );

    bud::asset::RawMesh combined_mesh{};
    combined_mesh.source_path = output_budasset_path;

    std::vector<std::string> ordered_link_names;
    std::unordered_map<std::string, uint32_t> link_to_bone_idx;
    ordered_link_names.push_back(root_link_name);
    link_to_bone_idx[root_link_name] = 0;

    for (const auto& link : robot_def.links) {
        if (link.name != root_link_name) {
            link_to_bone_idx[link.name] = static_cast<uint32_t>(ordered_link_names.size());
            ordered_link_names.push_back(link.name);
        }
    }

    struct RestVertexData {
        float pos[3];
        float bone_id;
        float normal[3];
        float pad = 0.0f;
    };
    std::vector<RestVertexData> bucket_rest_verts[4];

    auto get_material_slot = [](const std::string& link_name, const std::string& mesh_path) -> uint32_t {
        std::string s = link_name + " " + mesh_path;
        for (char& c : s) {
            c = static_cast<char>(std::tolower(c));
        }

        // 1: Dark Grey (waist, head, foot, logo - exactly matching official MuJoCo G1)
        if (s.find("waist") != std::string::npos ||
            s.find("head") != std::string::npos ||
            s.find("visor") != std::string::npos ||
            s.find("d435") != std::string::npos ||
            s.find("mid360") != std::string::npos ||
            s.find("foot") != std::string::npos ||
            s.find("ankle_roll") != std::string::npos ||
            s.find("logo") != std::string::npos)
        {
            return 1;
        }

        // 0: Bright Silver Metal (Torso, Pelvis, Arms, Hands, Thighs, Shins, Knees)
        return 0;
    };

    struct GeometryBucket {
        std::vector<bud::asset::RawVertex> vertices;
        std::vector<uint32_t> indices;
    };
    GeometryBucket buckets[4];

    for (const auto& link : robot_def.links) {
        auto it = link_xforms.find(link.name);
        if (it == link_xforms.end())
            continue;

        glm::mat4 link_xform = it->second;

        for (const auto& vis : link.visuals) {
            if (vis.geometry.mesh_path.empty())
                continue;

            glm::vec3 v_pos(vis.origin_xyz[0], vis.origin_xyz[1], vis.origin_xyz[2]);
            glm::quat v_rot = glm::angleAxis(vis.origin_rpy[2], glm::vec3(0.0f, 0.0f, 1.0f))
                            * glm::angleAxis(vis.origin_rpy[1], glm::vec3(0.0f, 1.0f, 0.0f))
                            * glm::angleAxis(vis.origin_rpy[0], glm::vec3(1.0f, 0.0f, 0.0f));

            glm::vec3 v_scale(
                vis.geometry.scale[0] != 0.0f ? vis.geometry.scale[0] : 1.0f,
                vis.geometry.scale[1] != 0.0f ? vis.geometry.scale[1] : 1.0f,
                vis.geometry.scale[2] != 0.0f ? vis.geometry.scale[2] : 1.0f
            );

            glm::mat4 vis_local = glm::translate(glm::mat4(1.0f), v_pos)
                                * glm::mat4_cast(v_rot)
                                * glm::scale(glm::mat4(1.0f), v_scale);

            glm::mat4 final_transform = basis_rot * link_xform * vis_local;
            glm::mat3 normal_matrix = glm::transpose(glm::inverse(glm::mat3(final_transform)));

            fs::path resolved_path = fs::path(package_root) / vis.geometry.mesh_path;
            auto part_raw_opt = load_raw_mesh_from_budasset_or_source(resolved_path.string(), 1.0f);
            if (!part_raw_opt) {
                part_raw_opt = load_raw_mesh_from_budasset_or_source(vis.geometry.mesh_path, 1.0f);
            }

            if (!part_raw_opt) {
                support::log_warn("[UrdfBuilder] Could not load visual mesh: " + resolved_path.string());
                continue;
            }

            uint32_t mat_slot = get_material_slot(link.name, vis.geometry.mesh_path);
            auto& target_bucket = buckets[mat_slot];
            uint32_t vertex_base = static_cast<uint32_t>(target_bucket.vertices.size());

            for (const auto& pv : part_raw_opt->vertices) {
                bud::asset::RawVertex nv = pv;
                glm::vec4 pos(pv.position[0], pv.position[1], pv.position[2], 1.0f);
                glm::vec4 xformed_pos = final_transform * pos;
                nv.position[0] = xformed_pos.x;
                nv.position[1] = xformed_pos.y;
                nv.position[2] = xformed_pos.z;

                glm::vec3 n(pv.normal[0], pv.normal[1], pv.normal[2]);
                glm::vec3 xformed_n = normal_matrix * n;
                float len = glm::length(xformed_n);
                if (len > 1e-6f)
                    xformed_n /= len;
                nv.normal[0] = xformed_n.x;
                nv.normal[1] = xformed_n.y;
                nv.normal[2] = xformed_n.z;

                target_bucket.vertices.push_back(nv);

                uint32_t bone_idx = link_to_bone_idx[link.name];
                RestVertexData rvd{};
                rvd.pos[0] = nv.position[0];
                rvd.pos[1] = nv.position[1];
                rvd.pos[2] = nv.position[2];
                rvd.bone_id = static_cast<float>(bone_idx);
                rvd.normal[0] = nv.normal[0];
                rvd.normal[1] = nv.normal[1];
                rvd.normal[2] = nv.normal[2];
                bucket_rest_verts[mat_slot].push_back(rvd);
            }

            for (uint32_t idx : part_raw_opt->indices) {
                target_bucket.indices.push_back(vertex_base + idx);
            }
        }
    }

    std::vector<RestVertexData> combined_rest_verts;

    // Assemble combined mesh from material buckets
    for (uint32_t slot = 0; slot < 4; ++slot) {
        if (buckets[slot].indices.empty())
            continue;

        uint32_t index_offset = static_cast<uint32_t>(combined_mesh.indices.size());
        uint32_t vertex_offset = static_cast<uint32_t>(combined_mesh.vertices.size());

        for (const auto& v : buckets[slot].vertices) {
            combined_mesh.vertices.push_back(v);
        }
        for (const auto& r : bucket_rest_verts[slot]) {
            combined_rest_verts.push_back(r);
        }
        for (uint32_t idx : buckets[slot].indices) {
            combined_mesh.indices.push_back(vertex_offset + idx);
        }

        bud::asset::RawSubmesh sub{};
        sub.name = robot_def.name + "_sub_" + std::to_string(slot);
        sub.index_offset = index_offset;
        sub.index_count = static_cast<uint32_t>(buckets[slot].indices.size());
        sub.material_index = slot;
        combined_mesh.submeshes.push_back(sub);
    }

    if (combined_mesh.vertices.empty() || combined_mesh.indices.empty()) {
        support::log_error("[UrdfBuilder] Combined mesh has no geometry!");
        return false;
    }

    // 4. Configure unified premium industrial PBR materials matching official MuJoCo G1
    // 0: Satin Metallic Silver (MuJoCo reference: bright diffuse-metallic, not mirror)
    //    In PBR, MuJoCo's simple Phong silver maps to: high albedo + moderate metallic + higher roughness
    bud::asset::RawMaterial mat_silver_shell{};
    mat_silver_shell.name = robot_def.name + "_silver_metal";
    mat_silver_shell.base_color_factor[0] = 0.85f;
    mat_silver_shell.base_color_factor[1] = 0.86f;
    mat_silver_shell.base_color_factor[2] = 0.87f;
    mat_silver_shell.base_color_factor[3] = 1.0f;
    mat_silver_shell.metallic_factor = 0.72f;
    mat_silver_shell.roughness_factor = 0.42f;
    mat_silver_shell.alpha_mode = bud::asset::AlphaMode::Opaque;
    mat_silver_shell.double_sided = false;

    // 1: Dark Grey Accent Mechanism (Waist, Head, Foot, Logo - matching official MuJoCo G1)
    bud::asset::RawMaterial mat_mechanism{};
    mat_mechanism.name = robot_def.name + "_dark_grey";
    mat_mechanism.base_color_factor[0] = 0.08f;
    mat_mechanism.base_color_factor[1] = 0.08f;
    mat_mechanism.base_color_factor[2] = 0.09f;
    mat_mechanism.base_color_factor[3] = 1.0f;
    mat_mechanism.metallic_factor = 0.20f;
    mat_mechanism.roughness_factor = 0.35f;
    mat_mechanism.alpha_mode = bud::asset::AlphaMode::Opaque;
    mat_mechanism.double_sided = false;

    // 2: Matte Carbon Rubber Grip (Hands & Foot Soles)
    bud::asset::RawMaterial mat_rubber{};
    mat_rubber.name = robot_def.name + "_rubber_carbon";
    mat_rubber.base_color_factor[0] = 0.04f;
    mat_rubber.base_color_factor[1] = 0.04f;
    mat_rubber.base_color_factor[2] = 0.045f;
    mat_rubber.base_color_factor[3] = 1.0f;
    mat_rubber.metallic_factor = 0.0f;
    mat_rubber.roughness_factor = 0.82f;
    mat_rubber.alpha_mode = bud::asset::AlphaMode::Opaque;
    mat_rubber.double_sided = false;

    // 3: High-Gloss Obsidian Visor & Logo (Head Helmet, LiDAR Sensors & UNITREE Text)
    bud::asset::RawMaterial mat_visor{};
    mat_visor.name = robot_def.name + "_visor_obsidian";
    mat_visor.base_color_factor[0] = 0.015f;
    mat_visor.base_color_factor[1] = 0.015f;
    mat_visor.base_color_factor[2] = 0.02f;
    mat_visor.base_color_factor[3] = 1.0f;
    mat_visor.metallic_factor = 0.1f;
    mat_visor.roughness_factor = 0.04f;
    mat_visor.alpha_mode = bud::asset::AlphaMode::Opaque;
    mat_visor.double_sided = false;

    combined_mesh.materials.push_back(mat_silver_shell);
    combined_mesh.materials.push_back(mat_mechanism);
    combined_mesh.materials.push_back(mat_rubber);
    combined_mesh.materials.push_back(mat_visor);

    combined_mesh.compute_bounds();

    // 5. Serialize into single .budasset container
    fs::path out_p(output_budasset_path);
    if (out_p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(out_p.parent_path(), ec);
    }

    uint64_t asset_id = compute_asset_id(output_budasset_path);
    BudAssetWriter writer(AssetType::Mesh, asset_id);
    std::vector<uint8_t> raw_bytes = combined_mesh.serialize_binary();
    writer.add_chunk(AssetChunkType::RawMesh, raw_bytes.data(), raw_bytes.size());

    // Pack skinning chunk inside the .budasset container
    if (!combined_rest_verts.empty() && !ordered_link_names.empty()) {
        std::vector<uint8_t> skin_bytes;
        auto write_skin = [&](const void* p, size_t sz) {
            const uint8_t* bp = reinterpret_cast<const uint8_t*>(p);
            skin_bytes.insert(skin_bytes.end(), bp, bp + sz);
        };

        uint32_t bone_cnt = static_cast<uint32_t>(ordered_link_names.size());
        uint32_t v_cnt = static_cast<uint32_t>(combined_rest_verts.size());
        write_skin(&bone_cnt, sizeof(bone_cnt));
        write_skin(&v_cnt, sizeof(v_cnt));

        for (const auto& b_name : ordered_link_names) {
            char name_buf[64] = { 0 };
            strncpy_s(name_buf, b_name.c_str(), sizeof(name_buf) - 1);
            write_skin(name_buf, sizeof(name_buf));

            glm::mat4 m = link_xforms.count(b_name) ? link_xforms[b_name] : glm::mat4(1.0f);
            write_skin(&m[0][0], sizeof(float) * 16);
        }

        write_skin(combined_rest_verts.data(), v_cnt * sizeof(RestVertexData));
        writer.add_chunk(AssetChunkType::Skinning, skin_bytes.data(), skin_bytes.size());
    }

    bool ok = writer.save_to_file(output_budasset_path);
    if (ok) {
        support::log_info("[UrdfBuilder] Successfully cooked unified robot asset (" +
                          std::to_string(combined_mesh.vertices.size()) + " verts, " +
                          std::to_string(combined_mesh.indices.size() / 3) + " tris) to: " + output_budasset_path);
    } else {
        support::log_error("[UrdfBuilder] Failed to write unified robot asset: " + output_budasset_path);
    }

    return ok;
}

} // namespace bud::asset_pipeline
