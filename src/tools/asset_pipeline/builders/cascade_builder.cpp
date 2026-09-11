#include "cascade_builder.hpp"
#include "texture_builder.hpp"
#include "material_asset_builder.hpp"
#include "mesh_builder.hpp"
#include "cloth_baker.hpp"
#include "../cache/asset_cache.hpp"
#include "../cache/asset_registry.hpp"
#include "../core/support.hpp"
#include "src/runtime/bud.scene.builder.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <chrono>
#include <nlohmann/json.hpp>
#include "../core/serializer.hpp"

namespace bud::asset_pipeline {

namespace {
    std::string resolve_scene_input_path(const std::string& input_path) {
        if (input_path.empty())
            return input_path;
        if (std::filesystem::exists(input_path))
            return input_path;

        std::filesystem::path p(input_path);
        if (!p.has_extension()) {
            if (std::filesystem::exists(input_path + ".json"))
                return input_path + ".json";
            if (std::filesystem::exists(input_path + ".budscene"))
                return input_path + ".budscene";
        }

        if (!p.has_parent_path()) {
            std::vector<std::string> candidates;
            if (p.has_extension()) {
                candidates.push_back((std::filesystem::path("Content") / input_path).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / input_path).generic_string());
            } else {
                candidates.push_back((std::filesystem::path("Content") / (input_path + ".json")).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / (input_path + ".budscene")).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / (input_path + ".json")).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / (input_path + ".budscene")).generic_string());
                candidates.push_back((std::filesystem::path("Content") / input_path).generic_string());
            }

            for (const auto& cand : candidates) {
                if (std::filesystem::exists(cand))
                    return cand;
            }

            return (std::filesystem::path("Content") / (p.has_extension() ? input_path : (input_path + ".json"))).generic_string();
        }

        return input_path;
    }

    RawMesh extract_submesh(const RawMesh& source_mesh, size_t sub_idx) {
        const auto& sub = source_mesh.submeshes[sub_idx];
        RawMesh out;
        out.source_path = source_mesh.source_path;
        std::string sname = sub.name.empty() ? ("submesh_" + std::to_string(sub_idx)) : sub.name;

        std::unordered_map<uint32_t, uint32_t> index_remap;
        out.indices.reserve(sub.index_count);
        out.vertices.reserve(sub.index_count);

        for (uint32_t i = 0; i < sub.index_count; ++i) {
            uint32_t old_idx = source_mesh.indices[sub.index_offset + i];
            auto it = index_remap.find(old_idx);
            if (it != index_remap.end()) {
                out.indices.push_back(it->second);
            } else {
                uint32_t new_idx = static_cast<uint32_t>(out.vertices.size());
                index_remap[old_idx] = new_idx;
                out.vertices.push_back(source_mesh.vertices[old_idx]);
                out.indices.push_back(new_idx);
            }
        }

        RawSubmesh out_sub;
        out_sub.name = sname;
        out_sub.index_offset = 0;
        out_sub.index_count = static_cast<uint32_t>(out.indices.size());
        out_sub.material_index = 0;
        out.submeshes.push_back(out_sub);

        // Copy only the material for this submesh.
        // Rebuild the texture list to contain only the paths referenced by this
        // material so MeshBuilder assigns correct 0-based texture indices into
        // the per-asset texture string table stored in the budasset.
        if (sub.material_index < source_mesh.materials.size()) {
            RawMaterial mat = source_mesh.materials[sub.material_index];

            auto add_tex = [&](const std::string& path) {
                if (path.empty()) return;
                if (std::find(out.textures.begin(), out.textures.end(), path) == out.textures.end())
                    out.textures.push_back(path);
            };
            add_tex(mat.base_color_texture_path);
            add_tex(mat.normal_texture_path);
            add_tex(mat.metallic_roughness_texture_path);
            add_tex(mat.emissive_texture_path);

            out.materials.push_back(std::move(mat));
        }

        out.compute_bounds();
        return out;
    }
}

bool CascadeBuilder::build_package_from_raw(
    const RawMesh& raw_mesh,
    const std::string& output_package_dir,
    const CascadeBuildOptions& options) {

    auto t_start = std::chrono::steady_clock::now();
    std::filesystem::path src_p(raw_mesh.source_path.empty() ? output_package_dir : raw_mesh.source_path);
    std::string stem = src_p.stem().string();

    support::log_info("[BudAssetPipeline] Starting Cascade Package Build for: " + stem + " -> " + output_package_dir);

    if (options.use_cache) {
        AssetCache::init(options.cache_root);
        AssetRegistry::init(options.cache_root + "/Registry/asset_registry.bin");
    }

    std::filesystem::path out_dir(output_package_dir);
    std::filesystem::path tex_dir = out_dir / "Textures";
    std::filesystem::path mat_dir = out_dir / "Materials";
    std::filesystem::path mesh_dir = out_dir / "Meshes";

    std::error_code ec;
    std::filesystem::create_directories(tex_dir, ec);
    std::filesystem::create_directories(mat_dir, ec);
    std::filesystem::create_directories(mesh_dir, ec);

    // 1. Textures
    std::vector<std::string> all_textures = raw_mesh.textures;
    auto add_unique_texture = [&](const std::string& p) {
        if (p.empty()) return;
        if (std::find(all_textures.begin(), all_textures.end(), p) == all_textures.end()) {
            all_textures.push_back(p);
        }
    };
    for (const auto& mat : raw_mesh.materials) {
        add_unique_texture(mat.base_color_texture_path);
        add_unique_texture(mat.normal_texture_path);
        add_unique_texture(mat.metallic_roughness_texture_path);
        add_unique_texture(mat.emissive_texture_path);
    }

    std::unordered_map<std::string, uint64_t> texture_id_map;
    std::hash<std::string> hasher;

    TextureBuildOptions tex_opts{};
    tex_opts.use_cache = options.use_cache;
    tex_opts.cache_root = options.cache_root;

    std::unordered_map<std::string, std::string> texture_cooked_paths;

    for (const auto& tex_path : all_textures) {
        if (tex_path.empty())
            continue;

        std::filesystem::path tp(tex_path);
        std::string tex_name = tp.stem().string();
        std::string out_tex_path = (tex_dir / (tex_name + ".budasset")).generic_string();

        uint64_t tex_id = hasher(tex_path);
        texture_id_map[tex_path] = tex_id;

        std::string resolved_path = tex_path;
        if (!std::filesystem::exists(resolved_path)) {
            const std::string extensions[] = { ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
            for (const auto& ext : extensions) {
                auto cand = std::filesystem::path(tex_path).replace_extension(ext).string();
                if (std::filesystem::exists(cand)) {
                    resolved_path = cand;
                    break;
                }
            }
        }

        if (std::filesystem::exists(resolved_path)) {
            if (TextureBuilder::build_from_file(resolved_path, out_tex_path, tex_opts)) {
                support::log_info("[BudAssetPipeline] Cooked Texture: " + resolved_path + " -> " + out_tex_path);

                AssetRegistryEntry entry;
                entry.asset_path = out_tex_path;
                entry.asset_id = tex_id;
                entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Texture);
                AssetRegistry::register_asset(entry);

                texture_cooked_paths[tex_path] = out_tex_path;
                texture_cooked_paths[resolved_path] = out_tex_path;
            }
        }
    }

    // Remap RawMesh textures and material texture paths to cooked .budasset paths
    RawMesh cooked_mesh = raw_mesh;
    for (auto& tex : cooked_mesh.textures) {
        if (texture_cooked_paths.contains(tex)) {
            tex = texture_cooked_paths[tex];
        }
    }
    for (auto& mat : cooked_mesh.materials) {
        if (texture_cooked_paths.contains(mat.base_color_texture_path))
            mat.base_color_texture_path = texture_cooked_paths[mat.base_color_texture_path];
        if (texture_cooked_paths.contains(mat.normal_texture_path))
            mat.normal_texture_path = texture_cooked_paths[mat.normal_texture_path];
        if (texture_cooked_paths.contains(mat.metallic_roughness_texture_path))
            mat.metallic_roughness_texture_path = texture_cooked_paths[mat.metallic_roughness_texture_path];
        if (texture_cooked_paths.contains(mat.emissive_texture_path))
            mat.emissive_texture_path = texture_cooked_paths[mat.emissive_texture_path];
    }

    // 2. Materials
    std::unordered_map<std::string, uint64_t> material_id_map;
    for (const auto& mat : cooked_mesh.materials) {
        std::string mat_name = mat.name.empty() ? "DefaultMaterial" : mat.name;
        std::string out_mat_path = (mat_dir / (mat_name + ".budasset")).generic_string();

        uint64_t mat_id = hasher(mat_name);
        material_id_map[mat_name] = mat_id;

        if (MaterialAssetBuilder::build(mat, texture_id_map, out_mat_path)) {
            support::log_info("[BudAssetPipeline] Cooked Material: " + mat_name + " -> " + out_mat_path);

            AssetRegistryEntry entry;
            entry.asset_path = out_mat_path;
            entry.asset_id = mat_id;
            entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Material);
            if (!mat.base_color_texture_path.empty()) {
                entry.dependencies.push_back(mat.base_color_texture_path);
            }
            AssetRegistry::register_asset(entry);
        }
    }

    // 3. Meshes (Decomposed per submesh)
    std::vector<bud::scene::Entity> entities;
    if (cooked_mesh.submeshes.size() > 1) {
        entities.reserve(cooked_mesh.submeshes.size());
        for (size_t sub_idx = 0; sub_idx < cooked_mesh.submeshes.size(); ++sub_idx) {
            const auto& sub = cooked_mesh.submeshes[sub_idx];
            // Human-readable name for the entity (may have duplicates in FBX instanced meshes)
            std::string sname_readable = sub.name.empty() ? (stem + "_submesh_" + std::to_string(sub_idx)) : sub.name;
            // File name MUST be unique: always append sub_idx to avoid collisions when multiple
            // FBX nodes reference the same aiMesh (same sub.name) with different baked transforms.
            std::string sname = sname_readable + "_" + std::to_string(sub_idx);
            std::string out_mesh_path = (mesh_dir / (sname + ".budasset")).generic_string();
            std::string temp_raw = (options.cache_root + "/" + sname + "_temp.rawmesh");


            RawMesh sub_raw = extract_submesh(cooked_mesh, sub_idx);

            // Apply optional uniform scale (same logic as the single-submesh branch).
            if (options.scale != 1.0f && options.scale > 0.0f) {
                for (auto& v : sub_raw.vertices) {
                    v.position[0] *= options.scale;
                    v.position[1] *= options.scale;
                    v.position[2] *= options.scale;
                }
                sub_raw.compute_bounds();
            }

            sub_raw.save_binary(temp_raw);

            bool is_translucent = false;
            if (sub.material_index < cooked_mesh.materials.size()) {
                const auto alpha = cooked_mesh.materials[sub.material_index].alpha_mode;
                // AlphaMode::Blend  → Translucent path (no VG, standard indexed draw).
                // AlphaMode::Mask   → stays VG; alpha discard is done in the VG fragment shader.
                // AlphaMode::Opaque → VG (default).
                if (alpha == bud::asset::AlphaMode::Blend) {
                    is_translucent = true;
                }
            }

            MeshBuildOptions mesh_opts{};
            mesh_opts.enable_virtual_geometry = !is_translucent;
            mesh_opts.dump_text = options.dump_text;
            mesh_opts.use_cache = options.use_cache;
            mesh_opts.cache_root = options.cache_root;

            if (MeshBuilder::build(temp_raw, out_mesh_path, mesh_opts)) {
                uint64_t asset_id = hasher(out_mesh_path);
                AssetRegistryEntry entry;
                entry.asset_path = out_mesh_path;
                entry.asset_id = asset_id;
                entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Mesh);
                if (sub.material_index < cooked_mesh.materials.size()) {
                    std::string mname = cooked_mesh.materials[sub.material_index].name.empty() ? "DefaultMaterial" : cooked_mesh.materials[sub.material_index].name;
                    entry.dependencies.push_back((mat_dir / (mname + ".budasset")).generic_string());
                }
                AssetRegistry::register_asset(entry);

                bud::scene::Entity ent;
                ent.name = options.entity_prefix.empty() ? sname_readable : (options.entity_prefix + "_" + sname_readable);
                ent.asset_path = out_mesh_path;
                ent.is_active = true;

                bool is_cloth_ent = false;
                if (sub.material_index < cooked_mesh.materials.size()) {
                    if (ClothBaker::is_cloth_material(cooked_mesh.materials[sub.material_index].name))
                        is_cloth_ent = true;
                }
                if (!is_cloth_ent && (ClothBaker::is_cloth_material(sname_readable) || ClothBaker::is_cloth_material(out_mesh_path)))
                    is_cloth_ent = true;

                ent.is_static = !is_cloth_ent;
                ent.mesh_index = 0;
                ent.material_index = 0;
                ent.transform = glm::mat4(1.0f);
                entities.push_back(ent);
            }
            std::filesystem::remove(temp_raw, ec);
        }
    } else {
        // Single submesh / mesh
        std::string out_mesh_path = (mesh_dir / (stem + ".budasset")).generic_string();
        std::string temp_raw = (options.cache_root + "/" + stem + "_temp.rawmesh");
		if (options.scale != 1.0f && options.scale > 0.0f) {
			for (auto& v : cooked_mesh.vertices) {
				v.position[0] *= options.scale;
				v.position[1] *= options.scale;
				v.position[2] *= options.scale;
			}
			cooked_mesh.compute_bounds();
			cooked_mesh.save_binary(temp_raw);
		} else {
			cooked_mesh.save_binary(temp_raw);
		}

		bool is_translucent = false;
		if (!cooked_mesh.materials.empty()) {
			const auto alpha = cooked_mesh.materials[0].alpha_mode;
			// AlphaMode::Blend  → Translucent path (no VG, standard indexed draw).
			// AlphaMode::Mask   → stays VG; alpha discard is done in the VG fragment shader.
			if (alpha == bud::asset::AlphaMode::Blend) {
				is_translucent = true;
			}
		}

        MeshBuildOptions mesh_opts{};
        mesh_opts.enable_virtual_geometry = !is_translucent;
        mesh_opts.dump_text = options.dump_text;
        mesh_opts.use_cache = options.use_cache;
        mesh_opts.cache_root = options.cache_root;

        if (MeshBuilder::build(temp_raw, out_mesh_path, mesh_opts)) {
			uint64_t asset_id = hasher(cooked_mesh.source_path.empty() ? out_mesh_path : cooked_mesh.source_path);

			AssetRegistryEntry entry;
            entry.asset_path = out_mesh_path;
            entry.asset_id = asset_id;
            entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Mesh);
            for (const auto& mat : cooked_mesh.materials) {
                std::string mname = mat.name.empty() ? "DefaultMaterial" : mat.name;
                entry.dependencies.push_back((mat_dir / (mname + ".budasset")).generic_string());
            }
            AssetRegistry::register_asset(entry);

            bud::scene::Entity ent;
            ent.name = options.entity_prefix.empty() ? stem : (options.entity_prefix + "_" + stem);
            ent.asset_path = out_mesh_path;
            ent.is_active = true;

            bool is_cloth_ent = false;
            for (const auto& mat : cooked_mesh.materials) {
                if (ClothBaker::is_cloth_material(mat.name)) {
                    is_cloth_ent = true;
                    break;
                }
            }
            if (!is_cloth_ent && (ClothBaker::is_cloth_material(stem) || ClothBaker::is_cloth_material(out_mesh_path)))
                is_cloth_ent = true;

            ent.is_static = !is_cloth_ent;
            ent.mesh_index = 0;
            ent.material_index = 0;
            ent.transform = glm::mat4(1.0f);
            entities.push_back(ent);
        }
        std::filesystem::remove(temp_raw, ec);
    }

    AssetRegistry::save();

    // Append to existing scene file if target_scene_path is provided
    if (options.scene_mode == SceneImportMode::Append || !options.target_scene_path.empty()) {
        std::string scene_file_path = resolve_scene_input_path(options.target_scene_path);
        bud::scene::Scene target_scene;
        if (!bud::scene::SceneBuilder::load_scene_from_file(scene_file_path, target_scene)) {
            support::log_error("[BudAssetImporter] Error: Target scene does not exist: " + scene_file_path +
                              ". Please create the scene first using SceneTool.");
            return false;
        }

        bud::scene::SceneBuilder::append_entities(target_scene, entities);

        if (bud::scene::SceneBuilder::save_scene_to_file(target_scene, scene_file_path)) {
            support::log_info("[BudAssetPipeline] Appended " + std::to_string(entities.size()) +
                              " submesh entities to target scene: " + scene_file_path);
        } else {
            support::log_error("[BudAssetPipeline] Failed to save updated target scene: " + scene_file_path);
            return false;
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    support::log_info("[BudAssetPipeline] Cascade package successfully built in " + std::to_string(ms) + " ms.");
    return true;
}

bool CascadeBuilder::build_package(
    const std::string& input_mesh_path,
    const std::string& output_package_dir,
    const CascadeBuildOptions& options) {

    std::filesystem::path mesh_p(input_mesh_path);
    std::string ext = mesh_p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    std::optional<RawMesh> raw_mesh_opt;
    if (ext == ".budasset") {
        std::ifstream in(input_mesh_path, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            std::streamsize fsize = in.tellg();
            in.seekg(0, std::ios::beg);
            std::vector<uint8_t> buf(static_cast<size_t>(fsize));
            if (in.read(reinterpret_cast<char*>(buf.data()), fsize)) {
                auto reader = BudAssetReader::load_from_memory(buf.data(), buf.size());
                if (reader) {
                    auto raw_chunk = reader->get_chunk_data(AssetChunkType::RawMesh);
                    if (raw_chunk && !raw_chunk->empty()) {
                        raw_mesh_opt = RawMesh::deserialize_binary(raw_chunk->data(), raw_chunk->size());
                    }
                }
            }
        }
    } else {
        raw_mesh_opt = RawMesh::load_from_file(input_mesh_path);
    }

    if (!raw_mesh_opt) {
        support::log_error("[BudAssetPipeline] Failed to load RawMesh from: " + input_mesh_path);
        return false;
    }

    return build_package_from_raw(*raw_mesh_opt, output_package_dir, options);
}

bool CascadeBuilder::build_scene_package(
    const RawScene& scene,
    const std::string& output_package_dir,
    const CascadeBuildOptions& options) {

    auto t_start = std::chrono::steady_clock::now();
    support::log_info("[BudAssetPipeline] Starting Scene Package Build with 1:1 Directory Mirroring for: " +
                      scene.name + " -> " + output_package_dir);

    if (options.use_cache) {
        AssetCache::init(options.cache_root);
        AssetRegistry::init(options.cache_root + "/Registry/asset_registry.bin");
    }

    std::error_code ec;
    std::filesystem::path out_pkg(output_package_dir);
    std::filesystem::create_directories(out_pkg, ec);

    std::string base_dir;
    size_t last_slash = scene.source_path.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        base_dir = scene.source_path.substr(0, last_slash + 1);
    }

    std::hash<std::string> hasher;

    // 1. Cook Textures preserving 1:1 relative source directories
    std::vector<std::string> all_textures = scene.textures;
    auto add_unique_texture = [&](const std::string& p) {
        if (p.empty()) return;
        if (std::find(all_textures.begin(), all_textures.end(), p) == all_textures.end()) {
            all_textures.push_back(p);
        }
    };
    for (const auto& mat : scene.materials) {
        add_unique_texture(mat.base_color_texture_path);
        add_unique_texture(mat.normal_texture_path);
        add_unique_texture(mat.metallic_roughness_texture_path);
        add_unique_texture(mat.emissive_texture_path);
    }

    std::unordered_map<std::string, uint64_t> texture_id_map;
    std::unordered_map<std::string, std::string> texture_cooked_paths;

    TextureBuildOptions tex_opts{};
    tex_opts.use_cache = options.use_cache;
    tex_opts.cache_root = options.cache_root;

    for (const auto& tex_path : all_textures) {
        if (tex_path.empty()) continue;

        std::filesystem::path tp(tex_path);
        std::string tex_name = tp.stem().string();

        // Extract relative directory from base_dir if possible
        std::filesystem::path rel_dir;
        if (!base_dir.empty() && tex_path.rfind(base_dir, 0) == 0) {
            std::filesystem::path rel = std::filesystem::relative(tex_path, base_dir, ec);
            if (!ec && rel.has_parent_path()) {
                rel_dir = rel.parent_path();
            }
        }

        std::filesystem::path target_tex_dir = out_pkg / rel_dir;
        std::filesystem::create_directories(target_tex_dir, ec);
        std::string out_tex_path = (target_tex_dir / (tex_name + ".budasset")).generic_string();

        uint64_t tex_id = hasher(tex_path);
        texture_id_map[tex_path] = tex_id;
        texture_cooked_paths[tex_path] = out_tex_path;

        std::string resolved_path = tex_path;
        if (!std::filesystem::exists(resolved_path)) {
            const std::string extensions[] = { ".dds", ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
            for (const auto& ext : extensions) {
                auto cand = std::filesystem::path(tex_path).replace_extension(ext).string();
                if (std::filesystem::exists(cand)) {
                    resolved_path = cand;
                    break;
                }
            }
        }

        if (std::filesystem::exists(resolved_path)) {
            if (TextureBuilder::build_from_file(resolved_path, out_tex_path, tex_opts)) {
                AssetRegistryEntry entry;
                entry.asset_path = out_tex_path;
                entry.asset_id = tex_id;
                entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Texture);
                AssetRegistry::register_asset(entry);
            }
        }
    }

    // 2. Cook Materials preserving 1:1 relative directories
    std::unordered_map<std::string, uint64_t> material_id_map;
    std::unordered_map<std::string, std::string> material_cooked_paths;

    for (const auto& mat : scene.materials) {
        std::string mat_name = mat.name.empty() ? "DefaultMaterial" : mat.name;

        std::filesystem::path rel_dir;
        if (!mat.base_color_texture_path.empty() && texture_cooked_paths.contains(mat.base_color_texture_path)) {
            std::filesystem::path tcp(texture_cooked_paths[mat.base_color_texture_path]);
            rel_dir = std::filesystem::relative(tcp.parent_path(), out_pkg, ec);
        }

        std::filesystem::path target_mat_dir = out_pkg / rel_dir;
        std::filesystem::create_directories(target_mat_dir, ec);
        std::string out_mat_path = (target_mat_dir / (mat_name + ".budasset")).generic_string();

        uint64_t mat_id = hasher(mat_name);
        material_id_map[mat_name] = mat_id;
        material_cooked_paths[mat_name] = out_mat_path;

        if (MaterialAssetBuilder::build(mat, texture_id_map, out_mat_path)) {
            AssetRegistryEntry entry;
            entry.asset_path = out_mat_path;
            entry.asset_id = mat_id;
            entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Material);
            if (!mat.base_color_texture_path.empty() && texture_cooked_paths.contains(mat.base_color_texture_path)) {
                entry.dependencies.push_back(texture_cooked_paths[mat.base_color_texture_path]);
            }
            AssetRegistry::register_asset(entry);
        }
    }

    // 3. Cook Unique Meshes preserving 1:1 relative directories
    std::vector<std::string> mesh_cooked_paths(scene.meshes.size());

    for (size_t mi = 0; mi < scene.meshes.size(); ++mi) {
        const auto& sm = scene.meshes[mi];
        std::filesystem::path target_mesh_dir = out_pkg / sm.relative_dir;
        std::filesystem::create_directories(target_mesh_dir, ec);
        std::string out_mesh_path = (target_mesh_dir / (sm.name + ".budasset")).generic_string();

        mesh_cooked_paths[mi] = out_mesh_path;

        RawMesh cooked_mesh = sm.mesh;
        for (auto& tex : cooked_mesh.textures) {
            if (texture_cooked_paths.contains(tex)) {
                tex = texture_cooked_paths[tex];
            }
        }
        for (auto& mat : cooked_mesh.materials) {
            if (texture_cooked_paths.contains(mat.base_color_texture_path))
                mat.base_color_texture_path = texture_cooked_paths[mat.base_color_texture_path];
            if (texture_cooked_paths.contains(mat.normal_texture_path))
                mat.normal_texture_path = texture_cooked_paths[mat.normal_texture_path];
            if (texture_cooked_paths.contains(mat.metallic_roughness_texture_path))
                mat.metallic_roughness_texture_path = texture_cooked_paths[mat.metallic_roughness_texture_path];
            if (texture_cooked_paths.contains(mat.emissive_texture_path))
                mat.emissive_texture_path = texture_cooked_paths[mat.emissive_texture_path];
        }

        std::string temp_raw = (options.cache_root + "/" + sm.name + "_temp.rawmesh");
        cooked_mesh.save_binary(temp_raw);

        MeshBuildOptions mesh_opts{};
        mesh_opts.enable_virtual_geometry = !sm.is_translucent; // Translucent meshes cooked as standard index meshes
        mesh_opts.dump_text = options.dump_text;
        mesh_opts.use_cache = options.use_cache;
        mesh_opts.cache_root = options.cache_root;

        if (MeshBuilder::build(temp_raw, out_mesh_path, mesh_opts)) {
            uint64_t asset_id = hasher(out_mesh_path);
            AssetRegistryEntry entry;
            entry.asset_path = out_mesh_path;
            entry.asset_id = asset_id;
            entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Mesh);
            for (const auto& sub : sm.mesh.submeshes) {
                if (sub.material_index < scene.materials.size()) {
                    const auto& m = scene.materials[sub.material_index];
                    if (material_cooked_paths.contains(m.name)) {
                        entry.dependencies.push_back(material_cooked_paths[m.name]);
                    }
                }
            }
            AssetRegistry::register_asset(entry);
        }
        std::filesystem::remove(temp_raw, ec);
    }

    AssetRegistry::save();

    // 4. Handle Scene File based on SceneImportMode
    if (options.scene_mode == SceneImportMode::None) {
        support::log_info("[BudAssetPipeline] Scene mode is 'None'. Skipping scene file generation.");
    } else {
        std::vector<bud::scene::Entity> entities;
        entities.reserve(scene.instances.size());

        for (const auto& inst : scene.instances) {
            if (inst.mesh_index >= mesh_cooked_paths.size() || mesh_cooked_paths[inst.mesh_index].empty())
                continue;

            bud::scene::Entity ent;
            std::string ename = options.entity_prefix.empty() ? inst.name : (options.entity_prefix + "_" + inst.name);
            ent.name = ename;
            ent.asset_path = mesh_cooked_paths[inst.mesh_index];
            ent.is_active = true;
            ent.is_static = true;
            ent.material_index = 0;
            ent.mesh_index = 0;

            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    ent.transform[r][c] = inst.transform[r * 4 + c];
                }
            }

            entities.push_back(ent);
        }

        // Append to existing scene file if target_scene_path is provided
        if (options.scene_mode == SceneImportMode::Append || !options.target_scene_path.empty()) {
            std::string scene_file_path = resolve_scene_input_path(options.target_scene_path);
            bud::scene::Scene target_scene;
            if (!bud::scene::SceneBuilder::load_scene_from_file(scene_file_path, target_scene)) {
                support::log_error("[BudAssetImporter] Error: Target scene does not exist: " + scene_file_path +
                                  ". Please create the scene first using SceneTool.");
                return false;
            }

            bud::scene::SceneBuilder::append_entities(target_scene, entities);
            if (bud::scene::SceneBuilder::save_scene_to_file(target_scene, scene_file_path)) {
                support::log_info("[BudAssetPipeline] Appended " + std::to_string(entities.size()) +
                                  " entities to target scene: " + scene_file_path);
            } else {
                support::log_error("[BudAssetPipeline] Failed to save updated target scene: " + scene_file_path);
                return false;
            }
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    support::log_info("[BudAssetPipeline] Scene Package Build finished in " + std::to_string(ms) + " ms.");

    return true;
}

} // namespace bud::asset_pipeline
