#include "cascade_builder.hpp"
#include "texture_builder.hpp"
#include "material_asset_builder.hpp"
#include "mesh_builder.hpp"
#include "../cache/asset_cache.hpp"
#include "../cache/asset_registry.hpp"
#include "../core/support.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <chrono>
#include "../core/serializer.hpp"

namespace bud::asset_pipeline {

bool CascadeBuilder::build_package_from_raw(
    const RawMesh& raw_mesh,
    const std::string& output_package_dir,
    const CascadeBuildOptions& options) {

    auto t_start = std::chrono::steady_clock::now();
    std::filesystem::path src_p(raw_mesh.source_path.empty() ? "Mesh" : raw_mesh.source_path);
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

    // 1. Cascade Build: Textures
    std::unordered_map<std::string, uint64_t> texture_id_map;
    std::hash<std::string> hasher;

    TextureBuildOptions tex_opts{};
    tex_opts.use_cache = options.use_cache;
    tex_opts.cache_root = options.cache_root;

    for (const auto& tex_path : raw_mesh.textures) {
        if (tex_path.empty())
            continue;

        std::filesystem::path tp(tex_path);
        std::string tex_name = tp.stem().string();
        std::string out_tex_path = (tex_dir / (tex_name + ".budasset")).string();

        uint64_t tex_id = hasher(tex_path);
        texture_id_map[tex_path] = tex_id;

        if (std::filesystem::exists(tex_path)) {
            if (TextureBuilder::build_from_file(tex_path, out_tex_path, tex_opts)) {
                support::log_info("[BudAssetPipeline] Cooked Texture: " + tex_path + " -> " + out_tex_path);

                AssetRegistryEntry entry;
                entry.asset_path = out_tex_path;
                entry.asset_id = tex_id;
                entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Texture);
                AssetRegistry::register_asset(entry);
            }
        }
    }

    // 2. Cascade Build: Materials
    std::unordered_map<std::string, uint64_t> material_id_map;
    for (const auto& mat : raw_mesh.materials) {
        std::string mat_name = mat.name.empty() ? "DefaultMaterial" : mat.name;
        std::string out_mat_path = (mat_dir / (mat_name + ".budasset")).string();

        uint64_t mat_id = hasher(mat_name);
        material_id_map[mat_name] = mat_id;

        if (MaterialAssetBuilder::build(mat, texture_id_map, out_mat_path)) {
            support::log_info("[BudAssetPipeline] Cooked Material: " + mat_name + " -> " + out_mat_path);

            AssetRegistryEntry entry;
            entry.asset_path = out_mat_path;
            entry.asset_id = mat_id;
            entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Material);
            if (!mat.base_color_texture_path.empty()) {
                std::filesystem::path tp(mat.base_color_texture_path);
                entry.dependencies.push_back((tex_dir / (tp.stem().string() + ".budasset")).string());
            }
            AssetRegistry::register_asset(entry);
        }
    }

    // 3. Cascade Build: Mesh (with embedded RawMesh Chunk)
    std::string out_mesh_path = (mesh_dir / (stem + ".budasset")).string();
    std::string temp_raw = (options.cache_root + "/Meshes/" + stem + "_temp.rawmesh");
    raw_mesh.save_binary(temp_raw);

    MeshBuildOptions mesh_opts{};
    mesh_opts.enable_virtual_geometry = true;
    mesh_opts.dump_text = options.dump_text;
    mesh_opts.use_cache = options.use_cache;
    mesh_opts.cache_root = options.cache_root;

    bool mesh_ok = MeshBuilder::build(temp_raw, out_mesh_path, mesh_opts);
    std::filesystem::remove(temp_raw, ec);

    if (mesh_ok) {
        uint64_t asset_id = hasher(raw_mesh.source_path.empty() ? out_mesh_path : raw_mesh.source_path);
        AssetRegistryEntry entry;
        entry.asset_path = out_mesh_path;
        entry.asset_id = asset_id;
        entry.asset_type = static_cast<uint32_t>(bud::asset::AssetType::Mesh);
        for (const auto& mat : raw_mesh.materials) {
            std::string mname = mat.name.empty() ? "DefaultMaterial" : mat.name;
            entry.dependencies.push_back((mat_dir / (mname + ".budasset")).string());
        }
        AssetRegistry::register_asset(entry);
        AssetRegistry::save();

        auto t_end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
        support::log_info("[BudAssetPipeline] Cascade package successfully built in " + std::to_string(ms) + " ms.");
        return true;
    }

    return false;
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

} // namespace bud::asset_pipeline
