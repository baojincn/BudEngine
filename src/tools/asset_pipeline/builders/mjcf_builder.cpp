#include "mjcf_builder.hpp"
#include "../importers/mjcf_parser.hpp"
#include "../importers/stl_importer.hpp"
#include "../core/serializer.hpp"
#include "../core/support.hpp"
#include "src/robots/bud.robot.mujoco.hpp"

#include <pugixml.hpp>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>

namespace fs = std::filesystem;

namespace bud::asset_pipeline {

namespace {

void ensure_jolt_initialized() {
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

std::optional<bud::robot::ConvexHullData> extract_convex_hull(const bud::asset::RawMesh& mesh, int max_vertices) {
    ensure_jolt_initialized();
    if (mesh.vertices.empty())
        return std::nullopt;

    JPH::Array<JPH::Vec3> in_positions;
    in_positions.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices)
        in_positions.push_back(JPH::Vec3(v.position[0], v.position[1], v.position[2]));

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
                } else {
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

bool MjcfBuilder::build_mjcf(
    const std::string& mjcf_path,
    const std::string& output_robot_dir,
    const MjcfBuildOptions& options
) {
    ensure_jolt_initialized();
    auto t_start = std::chrono::steady_clock::now();
    support::log_info("[MjcfBuilder] Starting native MJCF cooking: " + mjcf_path + " -> " + output_robot_dir);

    auto robot_def_opt = MjcfParser::parse_file(mjcf_path);
    if (!robot_def_opt) {
        support::log_error("[MjcfBuilder] Failed to parse MJCF: " + mjcf_path);
        return false;
    }

    bud::robot::RobotDef robot_def = std::move(*robot_def_opt);

    fs::path out_root(output_robot_dir);
    fs::path vis_dir = out_root / "meshes" / "visual";
    fs::path col_dir = out_root / "meshes" / "collision";

    std::error_code ec;
    fs::create_directories(vis_dir, ec);
    fs::create_directories(col_dir, ec);

    uint32_t visual_cooked_count = 0;
    uint32_t visual_cached_count = 0;
    uint32_t collision_cooked_count = 0;

    std::unordered_map<std::string, std::string> cooked_mesh_map;

    // 1. Cook Visual Meshes
    for (auto& link : robot_def.links) {
        for (auto& vis : link.visuals) {
            if (vis.geometry.type != bud::robot::GeometryType::Mesh || vis.geometry.mesh_path.empty())
                continue;

            std::string src_path = vis.geometry.mesh_path;
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
                auto raw_mesh_opt = StlImporter::import_from_file(src_path, effective_scale);
                if (raw_mesh_opt) {
                    uint64_t asset_id = compute_asset_id(rel_asset_path);
                    BudAssetWriter writer(AssetType::Mesh, asset_id);
                    std::vector<uint8_t> raw_data = raw_mesh_opt->serialize_binary();
                    writer.add_chunk(AssetChunkType::RawMesh, raw_data.data(), raw_data.size());

                    if (writer.save_to_file(disk_asset_path.string())) {
                        visual_cooked_count++;
                    } else {
                        support::log_warn("[MjcfBuilder] Failed to write visual .budasset: " + disk_asset_path.string());
                    }
                } else {
                    support::log_warn("[MjcfBuilder] Failed to import STL visual mesh: " + src_path);
                }
            }

            cooked_mesh_map[stem] = rel_asset_path;
            cooked_mesh_map[src_p.filename().string()] = rel_asset_path;
            vis.geometry.mesh_path = rel_asset_path;
        }
    }

    // 2. Cook Collision Hulls
    for (auto& link : robot_def.links) {
        for (auto& col : link.collisions) {
            if (col.geometry.type != bud::robot::GeometryType::Mesh || col.geometry.mesh_path.empty())
                continue;

            std::string src_path = col.geometry.mesh_path;
            fs::path src_p(src_path);
            std::string stem = src_p.stem().string();

            // Check if identical visual mesh exists (Deduplication)
            std::string vis_rel_asset = "meshes/visual/" + stem + ".budasset";
            if (fs::exists(out_root / vis_rel_asset)) {
                col.geometry.mesh_path = vis_rel_asset;
            } else {
                std::string col_rel_asset = "meshes/collision/" + stem + ".budasset";
                fs::path disk_asset_path = out_root / col_rel_asset;

                auto raw_mesh_opt = StlImporter::import_from_file(src_path, options.scale);
                if (raw_mesh_opt) {
                    auto hull_opt = extract_convex_hull(*raw_mesh_opt, options.max_convex_vertices);
                    if (hull_opt) {
                        col.convex_hull = std::move(*hull_opt);
                        collision_cooked_count++;

                        uint64_t asset_id = compute_asset_id(col_rel_asset);
                        BudAssetWriter writer(AssetType::Physics, asset_id);
                        std::vector<uint8_t> hull_bytes = serialize_convex_hull(col.convex_hull);
                        writer.add_chunk(AssetChunkType::Collision, hull_bytes.data(), hull_bytes.size());
                        std::vector<uint8_t> raw_data = raw_mesh_opt->serialize_binary();
                        writer.add_chunk(AssetChunkType::RawMesh, raw_data.data(), raw_data.size());
                        writer.save_to_file(disk_asset_path.string());
                    }
                }
                col.geometry.mesh_path = col_rel_asset;
            }
        }
    }

    // 3. Prepare Master Articulation Container
    std::string master_asset_path = (out_root / (robot_def.name + ".budasset")).string();
    uint64_t master_asset_id = compute_asset_id(robot_def.name + ".budasset");
    BudAssetWriter master_writer(AssetType::Articulation, master_asset_id);

    // Build AssetManifest
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

    // Add Articulation Chunk
    std::vector<uint8_t> robot_bin = robot_def.serialize_binary();
    master_writer.add_chunk(AssetChunkType::Articulation, robot_bin.data(), robot_bin.size());

    // 4. Cook PhysicsModel Chunk (Native MJCF text + mesh references)
    {
        bud::robots::MujocoModelData mujoco_data;
        mujoco_data.format = bud::robots::MujocoModelFormat::Mjcf;

        std::ifstream xml_in(mjcf_path, std::ios::binary);
        if (xml_in.is_open()) {
            std::stringstream buffer;
            buffer << xml_in.rdbuf();
            mujoco_data.model_payload = buffer.str();
        } else {
            support::log_error("[MjcfBuilder] Failed to read MJCF text from: " + mjcf_path);
            return false;
        }

        // Prefix sites and sensors in model_payload with robot_name to avoid collisions in multi-robot worlds
        pugi::xml_document doc;
        pugi::xml_parse_result res = doc.load_string(mujoco_data.model_payload.c_str());
        if (res) {
            const std::string robot_name = robot_def.name;
            const std::string old_site_name = "imu";
            const std::string new_site_name = robot_name + "_imu";

            auto rename_sites = [&](auto& self, pugi::xml_node parent) -> void {
                for (pugi::xml_node child : parent.children()) {
                    if (std::string_view(child.name()) == "site") {
                        pugi::xml_attribute a = child.attribute("name");
                        if (a && std::string_view(a.as_string()) == old_site_name) {
                            a.set_value(new_site_name.c_str());
                        }
                    }
                    self(self, child);
                }
            };
            rename_sites(rename_sites, doc.child("mujoco").child("worldbody"));

            pugi::xml_node sensor_root = doc.child("mujoco").child("sensor");
            if (sensor_root) {
                for (pugi::xml_node sensor : sensor_root.children()) {
                    pugi::xml_attribute site_attr = sensor.attribute("site");
                    if (site_attr && std::string_view(site_attr.as_string()) == old_site_name) {
                        site_attr.set_value(new_site_name.c_str());
                    }
                    pugi::xml_attribute obj_attr = sensor.attribute("objname");
                    if (obj_attr && std::string_view(obj_attr.as_string()) == old_site_name) {
                        obj_attr.set_value(new_site_name.c_str());
                    }
                    pugi::xml_attribute name_attr = sensor.attribute("name");
                    if (name_attr) {
                        std::string sname = name_attr.as_string();
                        if (sname.find(robot_name + "_") != 0) {
                            name_attr.set_value((robot_name + "_" + sname).c_str());
                        }
                    }
                }
            }

            std::ostringstream oss;
            doc.save(oss);
            mujoco_data.model_payload = oss.str();
        }

        // Determine meshdir
        std::string mesh_dir = "assets/";
        const std::string compiler_tag = "<compiler ";
        const size_t compiler_at = mujoco_data.model_payload.find(compiler_tag);
        if (compiler_at != std::string::npos) {
            const size_t meshdir_at = mujoco_data.model_payload.find("meshdir=\"", compiler_at);
            if (meshdir_at != std::string::npos) {
                const size_t value_at = meshdir_at + 9;
                const size_t value_end = mujoco_data.model_payload.find('"', value_at);
                if (value_end != std::string::npos) {
                    mesh_dir = mujoco_data.model_payload.substr(value_at, value_end - value_at);
                    if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
                        mesh_dir += "/";
                }
            }
        }

        // Map every <mesh file="..."> into MujocoMeshRef
        const std::string mesh_tag = "<mesh ";
        size_t cursor = 0;
        std::unordered_set<std::string> registered_meshes;
        while (true) {
            const size_t at = mujoco_data.model_payload.find(mesh_tag, cursor);
            if (at == std::string::npos)
                break;

            const size_t tag_end = mujoco_data.model_payload.find('>', at);
            const size_t file_at = mujoco_data.model_payload.find("file=\"", at);
            if (file_at == std::string::npos || file_at > tag_end) {
                cursor = tag_end + 1;
                continue;
            }

            const size_t val_start = file_at + 6;
            const size_t val_end = mujoco_data.model_payload.find('"', val_start);
            if (val_end == std::string::npos)
                break;

            cursor = val_end + 1;
            std::string file_name = mujoco_data.model_payload.substr(val_start, val_end - val_start);
            std::string stem = fs::path(file_name).stem().string();

            bud::robots::MujocoMeshRef ref{};
            ref.mjcf_name = mesh_dir + file_name;

            const auto it = cooked_mesh_map.find(stem);
            if (it != cooked_mesh_map.end())
                ref.asset_path = it->second;
            else
                ref.asset_path = "meshes/visual/" + stem + ".budasset";

            if (registered_meshes.insert(ref.mjcf_name).second)
                mujoco_data.meshes.push_back(std::move(ref));
        }

        std::vector<uint8_t> mujoco_blob = mujoco_data.serialize_binary();
        master_writer.add_chunk(AssetChunkType::PhysicsModel, mujoco_blob.data(), mujoco_blob.size());
    }

    if (!master_writer.save_to_file(master_asset_path)) {
        support::log_error("[MjcfBuilder] Failed to write master .budasset file: " + master_asset_path);
        return false;
    }

    if (options.dump_json) {
        std::string master_json_path = (out_root / (robot_def.name + ".budasset.json")).string();
        robot_def.save_json(master_json_path);
    }

    auto t_end = std::chrono::steady_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    std::cout << "==========================================================" << std::endl;
    std::cout << "[MjcfBuilder] Successfully Cooked Native MJCF Robot: " << robot_def.name << std::endl;
    std::cout << "  Root Link:        " << robot_def.root_link << std::endl;
    std::cout << "  Total Links:      " << robot_def.links.size() << std::endl;
    std::cout << "  Total Joints:     " << robot_def.joints.size() << std::endl;
    std::cout << "  Visual Meshes:    " << (visual_cooked_count + visual_cached_count) << " (" << visual_cooked_count << " cooked, " << visual_cached_count << " cached)" << std::endl;
    std::cout << "  Collision Hulls:  " << collision_cooked_count << std::endl;
    std::cout << "  Master Asset:     " << master_asset_path << " (AssetType::Articulation)" << std::endl;
    std::cout << "  Cook Duration:    " << duration_ms << " ms" << std::endl;
    std::cout << "==========================================================" << std::endl;

    return true;
}

} // namespace bud::asset_pipeline
