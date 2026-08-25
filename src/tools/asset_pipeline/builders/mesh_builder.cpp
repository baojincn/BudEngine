#include "mesh_builder.hpp"
#include "virtual_geometry_builder.hpp"
#include "material_builder.hpp"
#include "../cache/asset_cache.hpp"
#include "../core/serializer.hpp"
#include "../core/support.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace bud::asset_pipeline {

bool MeshBuilder::build(const std::string& input_path, const std::string& output_path, const MeshBuildOptions& options) {
    auto t_start = std::chrono::steady_clock::now();
    support::log_info("[BudAssetPipeline] Processing mesh: " + input_path + " -> " + output_path);

    std::filesystem::path p(input_path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext != ".rawmesh" && ext != ".budasset" && ext != ".json") {
        support::log_error("[BudAssetCompiler] Error: Input must be a .budasset or .rawmesh asset! Please run BudAssetImporter first.");
        return false;
    }

    if (options.use_cache) {
        AssetCache::init(options.cache_root);
    }

    std::filesystem::path out_p(output_path);
    std::string output_bulk_path = (out_p.parent_path() / (out_p.stem().string() + ".budbulk")).string();

    std::string build_key;
    constexpr uint32_t build_version = 11;
    build_key = AssetCache::compute_build_key(input_path, build_version);

    // 1. Check if Cooked .budasset and .budbulk exist in DDC cache
    if (options.use_cache && AssetCache::has_mesh_cache(build_key)) {
        bool bulk_ok = true;
        if (options.enable_virtual_geometry) {
            bulk_ok = AssetCache::has_mesh_bulk_cache(build_key);
        }

        if (bulk_ok) {
            std::string cached_cooked = AssetCache::get_mesh_cache_path(build_key);
            support::log_info("[BudAssetPipeline] DDC Cooked Cache Hit: " + cached_cooked);
            std::error_code ec;
            if (out_p.has_parent_path())
                std::filesystem::create_directories(out_p.parent_path(), ec);
            std::filesystem::copy_file(cached_cooked, output_path, std::filesystem::copy_options::overwrite_existing, ec);

            if (options.enable_virtual_geometry) {
                std::string cached_bulk = AssetCache::get_mesh_bulk_cache_path(build_key);
                std::filesystem::copy_file(cached_bulk, output_bulk_path, std::filesystem::copy_options::overwrite_existing, ec);
            }

            if (!ec) {
                auto t_end = std::chrono::steady_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
                support::log_info("[BudAssetPipeline] Instantly retrieved cooked asset in " + std::to_string(ms) + " ms.");
                return true;
            }
        }
    }

    // 2. Load RawMesh (either from .rawmesh or extracted from .budasset container)
    std::optional<RawMesh> raw_mesh_opt;
    if (ext == ".budasset") {
        std::ifstream in(input_path, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            support::log_error("[BudAssetPipeline] Failed to open .budasset: " + input_path);
            return false;
        }

        std::streamsize fsize = in.tellg();
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> file_buffer(static_cast<size_t>(fsize));
        if (!in.read(reinterpret_cast<char*>(file_buffer.data()), fsize)) {
            support::log_error("[BudAssetPipeline] Failed to read .budasset: " + input_path);
            return false;
        }

        auto reader = BudAssetReader::load_from_memory(file_buffer.data(), file_buffer.size());
        if (!reader) {
            support::log_error("[BudAssetPipeline] Failed to parse .budasset: " + input_path);
            return false;
        }

        auto raw_chunk = reader->get_chunk_data(AssetChunkType::RawMesh);
        if (!raw_chunk.has_value() || raw_chunk->empty()) {
            support::log_error("[BudAssetPipeline] No RawMesh chunk found in .budasset: " + input_path);
            return false;
        }

        raw_mesh_opt = RawMesh::deserialize_binary(raw_chunk->data(), raw_chunk->size());
        if (!raw_mesh_opt) {
            support::log_error("[BudAssetPipeline] Failed to deserialize RawMesh chunk from: " + input_path);
            return false;
        }
        support::log_info("[BudAssetPipeline] Successfully extracted embedded RawMesh chunk (" +
                          std::to_string(raw_chunk->size() / 1024) + " KB) from .budasset.");
    } else {
        raw_mesh_opt = RawMesh::load_from_file(input_path);
        if (!raw_mesh_opt) {
            support::log_error("[BudAssetPipeline] Failed to load RawMesh from: " + input_path);
            return false;
        }
    }

    // Convert Level 2 RawMesh to Level 3 InternalMesh
    InternalMesh internal_mesh = InternalMesh::from_raw_mesh(*raw_mesh_opt);

    // Compute stats
    size_t total_verts = 0;
    size_t total_tris = 0;
    for (const auto& sm : internal_mesh.submeshes) {
        total_verts += sm.vertices.size();
        total_tris += sm.indices.size() / 3;
    }
    support::log_info("[BudAssetPipeline] Parsed " + std::to_string(total_verts) +
                      " vertices, " + std::to_string(total_tris) + " triangles across " +
                      std::to_string(internal_mesh.submeshes.size()) + " submeshes.");

    // Compute asset ID
    std::hash<std::string> hasher;
    uint64_t asset_id = hasher(input_path);

    // Build .budasset container
    BudAssetWriter writer(AssetType::Mesh, asset_id);

    // 1. Embed RawMesh chunk for self-contained rebuild resilience
    std::vector<uint8_t> raw_mesh_bytes = raw_mesh_opt->serialize_binary();
    writer.add_chunk(AssetChunkType::RawMesh, raw_mesh_bytes.data(), raw_mesh_bytes.size());

    // 2. Build Material chunk
    auto mat_res = MaterialBuilder::build(internal_mesh.materials, internal_mesh.textures);
    writer.add_chunk(AssetChunkType::Material, mat_res.serialized_chunk.data(), mat_res.serialized_chunk.size());

    // 3. Build Virtual Geometry chunk
    if (options.enable_virtual_geometry) {
        VGBuildResult vg_result = VirtualGeometryBuilder::build(internal_mesh);

        // Store 100% of raw 128KB geometry pages into external .budbulk
        std::vector<uint8_t> bulk_pages_data;
        uint64_t bulk_offset = 0;

        for (size_t i = 0; i < vg_result.pages.size(); ++i) {
            auto& page_state = vg_result.pages[i];
            const auto& pdata = vg_result.raw_page_data[i];

            page_state.flags = bud::asset::VG_PAGE_FLAG_STREAMABLE;
            page_state.raw_vertex_offset = static_cast<uint32_t>(bulk_offset);
            bulk_pages_data.insert(bulk_pages_data.end(), pdata.begin(), pdata.end());
            bulk_offset += pdata.size();
        }

        if (!bulk_pages_data.empty()) {
            std::error_code ec;
            if (out_p.has_parent_path())
                std::filesystem::create_directories(out_p.parent_path(), ec);

            std::ofstream bulk_out(output_bulk_path, std::ios::binary);
            if (bulk_out.is_open()) {
                bulk_out.write(reinterpret_cast<const char*>(bulk_pages_data.data()), bulk_pages_data.size());
                bulk_out.flush();
                support::log_info("[BudAssetPipeline] Saved bulk page stream (" +
                                  std::to_string(bulk_pages_data.size() / 1024) + " KB) to: " + output_bulk_path);
            }
            writer.set_flags(bud::asset::BUD_ASSET_FLAG_HAS_BULK_DATA);
            writer.set_bulk_data_size(bulk_pages_data.size());
        }

        // Layout VirtualGeometry Chunk payload (Pure DAG hierarchy + Pagetable + Materials + Textures)
        uint64_t off = sizeof(bud::asset::VGHeader);
        const uint64_t cluster_offset = off;
        off += vg_result.clusters.size() * sizeof(bud::asset::VGCluster);
        const uint64_t group_offset = off;
        off += vg_result.groups.size() * sizeof(bud::asset::VGClusterGroup);
        const uint64_t page_state_offset = off;
        off += vg_result.pages.size() * sizeof(bud::asset::VGPageStreamingState);
        const uint64_t dependency_offset = off;
        off += vg_result.dependencies.size() * sizeof(bud::asset::VGPageDependency);
        const uint64_t material_offset = off;
        off += internal_mesh.materials.size() * sizeof(bud::asset::MaterialDescriptor);
        const uint64_t texture_offset = off;
        size_t tex_string_table_size = 0;
        for (const auto& t : internal_mesh.textures) {
            tex_string_table_size += t.size() + 1;
        }
        off += tex_string_table_size;
        const uint64_t page_data_offset = off;

        std::vector<bud::asset::MaterialDescriptor> mat_descs(internal_mesh.materials.size());
        for (size_t mi = 0; mi < internal_mesh.materials.size(); ++mi) {
            const auto& src_m = internal_mesh.materials[mi];
            mat_descs[mi].alpha_mode = static_cast<uint32_t>(src_m.alpha_mode);
            mat_descs[mi].alpha_cutoff = src_m.alpha_cutoff;
            mat_descs[mi].double_sided = src_m.double_sided ? 1 : 0;
            mat_descs[mi].base_color_texture = 0;
            for (size_t ti = 0; ti < internal_mesh.textures.size(); ++ti) {
                if (internal_mesh.textures[ti] == src_m.base_color_texture_path) {
                    mat_descs[mi].base_color_texture = static_cast<uint32_t>(ti);
                    break;
                }
            }
        }

        size_t total_vg_size = static_cast<size_t>(page_data_offset);
        std::vector<uint8_t> vg_chunk(total_vg_size, 0);

        vg_result.header.cluster_offset = cluster_offset;
        vg_result.header.group_offset = group_offset;
        vg_result.header.page_state_offset = page_state_offset;
        vg_result.header.dependency_offset = dependency_offset;
        vg_result.header.material_offset = material_offset;
        vg_result.header.texture_offset = texture_offset;
        vg_result.header.material_count = static_cast<uint32_t>(internal_mesh.materials.size());
        vg_result.header.texture_count = static_cast<uint32_t>(internal_mesh.textures.size());
        vg_result.header.page_data_offset = page_data_offset;

        uint8_t* dst = vg_chunk.data();
        std::memcpy(dst, &vg_result.header, sizeof(bud::asset::VGHeader));
        dst += sizeof(bud::asset::VGHeader);

        if (!vg_result.clusters.empty()) {
            std::memcpy(dst, vg_result.clusters.data(), vg_result.clusters.size() * sizeof(bud::asset::VGCluster));
            dst += vg_result.clusters.size() * sizeof(bud::asset::VGCluster);
        }

        if (!vg_result.groups.empty()) {
            std::memcpy(dst, vg_result.groups.data(), vg_result.groups.size() * sizeof(bud::asset::VGClusterGroup));
            dst += vg_result.groups.size() * sizeof(bud::asset::VGClusterGroup);
        }

        if (!vg_result.pages.empty()) {
            std::memcpy(dst, vg_result.pages.data(), vg_result.pages.size() * sizeof(bud::asset::VGPageStreamingState));
            dst += vg_result.pages.size() * sizeof(bud::asset::VGPageStreamingState);
        }

        if (!vg_result.dependencies.empty()) {
            std::memcpy(dst, vg_result.dependencies.data(), vg_result.dependencies.size() * sizeof(bud::asset::VGPageDependency));
            dst += vg_result.dependencies.size() * sizeof(bud::asset::VGPageDependency);
        }

        if (!mat_descs.empty()) {
            std::memcpy(dst, mat_descs.data(), mat_descs.size() * sizeof(bud::asset::MaterialDescriptor));
            dst += mat_descs.size() * sizeof(bud::asset::MaterialDescriptor);
        }

        for (const auto& t : internal_mesh.textures) {
            std::memcpy(dst, t.c_str(), t.size() + 1);
            dst += t.size() + 1;
        }

        writer.add_chunk(AssetChunkType::VirtualGeometry, vg_chunk.data(), vg_chunk.size());

        if (options.dump_text) {
            std::string json_path = output_path + ".json";
            if (VirtualGeometryBuilder::dump_json(vg_result, internal_mesh, json_path)) {
                support::log_info("[BudAssetPipeline] Dumped text representation to: " + json_path);
            }
        }

        support::log_info("[BudAssetPipeline] Generated Virtual Geometry: " +
                          std::to_string(vg_result.clusters.size()) + " clusters, " +
                          std::to_string(vg_result.groups.size()) + " groups, " +
                          std::to_string(vg_result.pages.size()) + " pages (Metadata: " +
                          std::to_string(total_vg_size / 1024) + " KB, Bulk: " +
                          std::to_string(bulk_pages_data.size() / 1024) + " KB).");
    }

    // Save to output file
    if (!writer.save_to_file(output_path)) {
        support::log_error("[BudAssetPipeline] Failed to write container to: " + output_path);
        return false;
    }

    // Save to DDC Cooked Cache
    if (options.use_cache && !build_key.empty()) {
        std::string cached_cooked = AssetCache::get_mesh_cache_path(build_key);
        std::error_code ec;
        std::filesystem::copy_file(output_path, cached_cooked, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            support::log_info("[BudAssetPipeline] Saved cooked asset to DDC: " + cached_cooked);
        }

        if (std::filesystem::exists(output_bulk_path)) {
            std::string cached_bulk = AssetCache::get_mesh_bulk_cache_path(build_key);
            std::filesystem::copy_file(output_bulk_path, cached_bulk, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                support::log_info("[BudAssetPipeline] Saved cooked bulk to DDC: " + cached_bulk);
            }
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    support::log_info("[BudAssetPipeline] Successfully cooked " + output_path + " in " + std::to_string(ms) + " ms.");
    return true;
}

} // namespace bud::asset_pipeline
