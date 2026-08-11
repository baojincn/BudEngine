#include "bud.asset.processor.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cfloat>
#include <limits>
#include <map>
#include <unordered_map>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <meshoptimizer.h>
#include <optional>

#include "../bud_tool_support/bud_tool_support.hpp"
#if defined(__has_include)
# if __has_include(<spirv_reflect.h>)
#  ifndef SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
#   define SPIRV_REFLECT_USE_SYSTEM_SPIRV_H 1
#  endif
#  include <spirv_reflect.h>
#  define BUD_HAVE_SPIRV_REFLECT 1
# endif
#endif
#include <filesystem>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <functional>
// note: avoid depending on bud::io; use local helpers above

#if defined(BUD_HAVE_SPIRV_REFLECT)
static bool compile_shader_with_glslc(const std::filesystem::path& src, const std::filesystem::path& out_spv);
static bool reflect_and_validate_spv(const std::filesystem::path& spv_path);
#include <sstream>

static bool compile_shader_with_glslc(const std::filesystem::path& src, const std::filesystem::path& out_spv) {
    // Use process runner to capture output instead of manual redirection
    std::ostringstream cmd;
    cmd << "glslc " << '"' << src.string() << '"' << " -o " << '"' << out_spv.string() << '"';
    auto res = bud::tool_support::run_process_capture(cmd.str());
    if (!res.stderr_str.empty()) {
        // write compiler log next to spv
        std::filesystem::path log_path = out_spv;
        log_path += ".log";
        bud::tool_support::write_text_file_atomic(log_path, res.stderr_str);
    }
    return res.exit_code == 0;
}

static bool reflect_and_validate_spv(const std::filesystem::path& spv_path) {
    // Minimal reflection validation: attempt to create and destroy a module
    std::ifstream in(spv_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return false;
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<char> data(size);
    in.read(data.data(), size);
    SpvReflectShaderModule module;
    SpvReflectResult res = spvReflectCreateShaderModule(data.size(), data.data(), &module);
    if (res != SPV_REFLECT_RESULT_SUCCESS) return false;
    spvReflectDestroyShaderModule(&module);
    return true;
}
#endif

namespace bud::tool {

    bool AssetProcessor::process_gltf_to_budmesh(const std::string& input_path, const std::string& output_path,
                                                 size_t max_vertices, size_t max_triangles, float cone_weight,
                                                 size_t page_size) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(input_path, 
            aiProcess_Triangulate | 
            aiProcess_FlipUVs | 
            aiProcess_GenNormals | 
            aiProcess_CalcTangentSpace |
            aiProcess_JoinIdenticalVertices);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            std::cerr << "[Assimp Error]: " << importer.GetErrorString() << std::endl;
            return false;
        }

        if (scene->mNumMeshes == 0) {
            std::cerr << "[BudAssetTool] No meshes found in file." << std::endl;
            return false;
        }

        std::cout << "[BudAssetTool] Original Assimp Submesh Count: " << scene->mNumMeshes << std::endl;

        // 1. Process all meshes and generate meshlets per submesh
        std::vector<asset::Vertex> all_vertices;
        std::vector<uint32_t> all_indices;
        std::vector<asset::MeshletDescriptor> all_meshlets;
        std::vector<asset::MeshletCullData> all_cull_data;
        std::vector<uint32_t> all_meshlet_vertices;
        std::vector<uint32_t> all_meshlet_triangles;
        std::vector<asset::SubMeshDescriptor> submeshes;
        // Parallel per-cluster LOD metadata (Nanite-style): emitted alongside
        // all_meshlets so page writing can build PageClusterDesc.
        std::vector<uint32_t> all_cluster_lod;
        std::vector<float> all_cluster_error;
        // Which source mesh each cluster belongs to. Nanite-style LOD is a
        // per-mesh DAG (a mesh's LOD0..LOD2 clusters are one simplification
        // chain); coarse pages must therefore only group clusters of a SINGLE
        // mesh, never stitch clusters from different meshes.
        std::vector<uint32_t> all_cluster_mesh;


        std::string input_path_str = std::string(input_path);
        std::string base_dir = "";
        size_t last_slash = input_path_str.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            base_dir = input_path_str.substr(0, last_slash + 1);
        }

        std::vector<std::string> texture_paths;
        std::map<unsigned int, uint32_t> mat_to_tex_idx;
        std::map<unsigned int, uint32_t> mat_to_mat_idx;
        std::vector<asset::MaterialDescriptor> materials;

        uint32_t default_tex_idx = 0;
        texture_paths.push_back("data/textures/default.png");

        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            aiMaterial* mat = scene->mMaterials[i];
            aiString tex_path;

            uint32_t base_tex = default_tex_idx;
            if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) == AI_SUCCESS) {
                std::string p = tex_path.C_Str();
                if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) {
                    p = base_dir + p;
                }
                base_tex = (uint32_t)texture_paths.size();
                texture_paths.push_back(p);
            }

            // Default material descriptor
            asset::MaterialDescriptor md = {};
            md.base_color_texture = base_tex;
			md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::Opaque);
            md.double_sided = 0;
            md.alpha_cutoff = 0.5f;

            // Try to query two-sided and opacity from Assimp material (best-effort)
            int two_sided = 0;
            float opacity = 1.0f;
            if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS) {
                md.double_sided = two_sided ? 1 : 0;
            }
            if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
                if (opacity < 1.0f) {
                    // If an explicit opacity map exists, treat as MASK; otherwise BLEND
                    aiString op_tex;
                    if (mat->GetTexture(aiTextureType_OPACITY, 0, &op_tex) == AI_SUCCESS) {
						md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::Mask);
                    } else {
						md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::Blend);
                    }
                    md.alpha_cutoff = 0.5f;
                }
            }

            uint32_t mat_out_idx = (uint32_t)materials.size();
            materials.push_back(md);
            mat_to_mat_idx[i] = mat_out_idx;

            // Keep a mapping for diffuse texture for backwards compat if needed
            mat_to_tex_idx[i] = base_tex;
        }

        struct MeshInstance {
            unsigned int mesh_index;
            aiMatrix4x4 transform;
            std::string node_name;
        };

        std::vector<MeshInstance> instances;
        std::function<void(aiNode*, aiMatrix4x4, int)> collect_instances = [&](aiNode* node, aiMatrix4x4 parent_transform, int depth) {
            aiMatrix4x4 current_transform = parent_transform * node->mTransformation;
            for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
                instances.push_back({ node->mMeshes[i], current_transform, node->mName.C_Str() });
            }
            for (unsigned int i = 0; i < node->mNumChildren; ++i) {
                collect_instances(node->mChildren[i], current_transform, depth + 1);
            }
        };

        aiMatrix4x4 root_transform = aiMatrix4x4(); 
        collect_instances(scene->mRootNode, root_transform, 0);

        std::cout << "[BudAssetTool] Processing " << instances.size() << " instances..." << std::endl;
        for (size_t i = 0; i < instances.size(); ++i) {
            const auto& instance = instances[i];
            const aiMesh* mesh = scene->mMeshes[instance.mesh_index];
            unsigned int mat_idx = mesh->mMaterialIndex;
            uint32_t mapped_mat_idx = mat_to_mat_idx[mat_idx];

            std::vector<asset::Vertex> group_vertices;
            std::vector<uint32_t> group_indices;

            uint32_t base_v = 0;
            for (unsigned int v_idx = 0; v_idx < mesh->mNumVertices; ++v_idx) {
                asset::Vertex v = {};
                aiVector3D pos = instance.transform * mesh->mVertices[v_idx];
                v.position[0] = pos.x;
                v.position[1] = pos.y;
                v.position[2] = pos.z;

                if (mesh->HasNormals()) {
                    aiMatrix3x3 normal_matrix(instance.transform);
                    aiVector3D norm = normal_matrix * mesh->mNormals[v_idx];
                    norm.Normalize();
                    v.normal[0] = norm.x;
                    v.normal[1] = norm.y;
                    v.normal[2] = norm.z;
                }
                if (mesh->HasTextureCoords(0)) {
                    v.uv[0] = mesh->mTextureCoords[0][v_idx].x;
                    v.uv[1] = mesh->mTextureCoords[0][v_idx].y;
                }
                group_vertices.push_back(v);
            }

            for (unsigned int f_idx = 0; f_idx < mesh->mNumFaces; ++f_idx) {
                const aiFace& face = mesh->mFaces[f_idx];
                if (face.mNumIndices != 3) continue;
                group_indices.push_back(face.mIndices[0]);
                group_indices.push_back(face.mIndices[1]);
                group_indices.push_back(face.mIndices[2]);
            }

            if (group_indices.empty()) continue;

            // Global stats for this group relative to file
            uint32_t group_base_vertex = (uint32_t)all_vertices.size();
            uint32_t group_base_index = (uint32_t)all_indices.size();
            uint32_t group_base_meshlet = (uint32_t)all_meshlets.size();

            // Meshoptimizer processing
            std::vector<uint32_t> optimized_indices(group_indices.size());
            meshopt_optimizeVertexCache(optimized_indices.data(), group_indices.data(), group_indices.size(), group_vertices.size());

            // Append to global buffers MUST BE AFTER OPTIMIZATION AND USE OPTIMIZED_INDICES
            for (const auto& v : group_vertices) all_vertices.push_back(v);
            for (auto idx : optimized_indices) all_indices.push_back(group_base_vertex + idx);

            // --- Nanite-style multi-level LOD generation ---
            // LOD 0 = original meshlet set; LOD 1.. = meshopt_simplify then
            // rebuild meshlets. Every cluster keeps its LOD level and the
            // accumulated simplification error (object-space, relative to LOD0).
            const uint32_t LOD_COUNT = 3; // LOD0 (full), LOD1, LOD2
            const float lod_target_errors[LOD_COUNT] = { 0.0f, 1e-3f, 5e-3f };

            // Per-LOD meshlet generation: produce meshlets from a triangle list.
            // meshlet_sets[level] = (meshlets, vertices, triangles)
            std::vector<std::vector<meshopt_Meshlet>> lod_meshlets;
            std::vector<std::vector<unsigned int>> lod_meshlet_vertices;
            std::vector<std::vector<unsigned char>> lod_meshlet_triangles;

            // Simplification result per level (indices into group_vertices).
            std::vector<unsigned int> lod_indices = optimized_indices;
            float lod_error = 0.0f;

            for (uint32_t lod = 0; lod < LOD_COUNT; ++lod) {
                // For LOD>0, simplify the previous level's indices.
                if (lod > 0) {
                    const float target = lod_target_errors[lod];
                    float target_error = target;
                    size_t target_index_count = lod_indices.size() / 2; // 50% reduction per level
                    if (target_index_count < 3) target_index_count = 3;
                    std::vector<unsigned int> simplified(lod_indices.size());
                    float result_error = 0.0f;
                    size_t simplified_count = meshopt_simplify(
                        simplified.data(), lod_indices.data(), lod_indices.size(),
                        &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex),
                        target_index_count, target_error, 0, &result_error);
                    simplified.resize(simplified_count);
                    lod_indices.swap(simplified);
                    lod_error = result_error;
                }

                size_t max_meshlets = meshopt_buildMeshletsBound(lod_indices.size(), max_vertices, max_triangles);
                std::vector<meshopt_Meshlet> local_meshlets(max_meshlets);
                std::vector<unsigned int> local_meshlet_vertices(max_meshlets * max_vertices);
                std::vector<unsigned char> local_meshlet_triangles(max_meshlets * max_triangles * 3);

                size_t meshlet_count = meshopt_buildMeshlets(local_meshlets.data(), local_meshlet_vertices.data(), local_meshlet_triangles.data(),
                                                             lod_indices.data(), lod_indices.size(), &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex),
                                                             max_vertices, max_triangles, cone_weight);
                local_meshlets.resize(meshlet_count);
                lod_meshlets.push_back(std::move(local_meshlets));
                lod_meshlet_vertices.push_back(std::move(local_meshlet_vertices));
                lod_meshlet_triangles.push_back(std::move(local_meshlet_triangles));
            }

            // Total clusters across all LOD levels.
            uint32_t total_clusters = 0;
            for (const auto& set : lod_meshlets) total_clusters += (uint32_t)set.size();

            asset::SubMeshDescriptor sub_desc = {};
            sub_desc.index_start = group_base_index;
            sub_desc.index_count = (uint32_t)group_indices.size();
            sub_desc.meshlet_start = group_base_meshlet;
            sub_desc.meshlet_count = total_clusters;
            sub_desc.material_id = mapped_mat_idx;
            
            // Compute SubMesh AABB
            sub_desc.aabb_min[0] = sub_desc.aabb_min[1] = sub_desc.aabb_min[2] = std::numeric_limits<float>::max();
            sub_desc.aabb_max[0] = sub_desc.aabb_max[1] = sub_desc.aabb_max[2] = -std::numeric_limits<float>::max();
            for (const auto& v : group_vertices) {
                sub_desc.aabb_min[0] = std::min(sub_desc.aabb_min[0], v.position[0]);
                sub_desc.aabb_min[1] = std::min(sub_desc.aabb_min[1], v.position[1]);
                sub_desc.aabb_min[2] = std::min(sub_desc.aabb_min[2], v.position[2]);
                sub_desc.aabb_max[0] = std::max(sub_desc.aabb_max[0], v.position[0]);
                sub_desc.aabb_max[1] = std::max(sub_desc.aabb_max[1], v.position[1]);
                sub_desc.aabb_max[2] = std::max(sub_desc.aabb_max[2], v.position[2]);
            }
            submeshes.push_back(sub_desc);

            // Emit clusters for every LOD level. cluster_error is the accumulated
            // simplification error (object-space) for that level; parent_error
            // chains to the coarser level (level+1) so the GPU can transition.
            for (uint32_t lod = 0; lod < LOD_COUNT; ++lod) {
                float this_error = (lod == 0) ? 0.0f : lod_error;
                float parent_error = (lod + 1 < LOD_COUNT) ? lod_target_errors[lod + 1] : lod_target_errors[LOD_COUNT - 1];
                auto& local_meshlets = lod_meshlets[lod];
                auto& local_meshlet_vertices = lod_meshlet_vertices[lod];
                auto& local_meshlet_triangles = lod_meshlet_triangles[lod];

                for (size_t i = 0; i < local_meshlets.size(); ++i) {
                    meshopt_Meshlet& m = local_meshlets[i];
                    meshopt_optimizeMeshlet(&local_meshlet_vertices[m.vertex_offset], &local_meshlet_triangles[m.triangle_offset], m.triangle_count, m.vertex_count);

                    asset::MeshletDescriptor desc = {};
                    desc.vertex_offset = (uint32_t)all_meshlet_vertices.size();
                    desc.vertex_count = m.vertex_count;
                    desc.triangle_offset = (uint32_t)all_meshlet_triangles.size();
                    desc.triangle_count = m.triangle_count;
                    all_meshlets.push_back(desc);
                    all_cluster_lod.push_back(lod);
                    all_cluster_error.push_back(this_error);
                    all_cluster_mesh.push_back((uint32_t)i); // mesh (instance) ownership

                    for (uint32_t v_idx = 0; v_idx < m.vertex_count; ++v_idx) {
                        all_meshlet_vertices.push_back(group_base_vertex + local_meshlet_vertices[m.vertex_offset + v_idx]);
                    }
                    for (uint32_t t_idx = 0; t_idx < m.triangle_count * 3; ++t_idx) {
                        all_meshlet_triangles.push_back(local_meshlet_triangles[m.triangle_offset + t_idx]);
                    }

                    meshopt_Bounds mbounds = meshopt_computeMeshletBounds(&local_meshlet_vertices[m.vertex_offset], &local_meshlet_triangles[m.triangle_offset],
                                                                        m.triangle_count, &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex));
                    asset::MeshletCullData cull = {};
                    cull.bounding_sphere[0] = mbounds.center[0];
                    cull.bounding_sphere[1] = mbounds.center[1];
                    cull.bounding_sphere[2] = mbounds.center[2];
                    cull.bounding_sphere[3] = mbounds.radius;
                    cull.cone_axis[0] = mbounds.cone_axis_s8[0];
                    cull.cone_axis[1] = mbounds.cone_axis_s8[1];
                    cull.cone_axis[2] = mbounds.cone_axis_s8[2];
                    cull.cone_cutoff = mbounds.cone_cutoff_s8;
                    all_cull_data.push_back(cull);
                }
            }
        }

        // 2. Serialize to .budmesh
        std::ofstream out(output_path, std::ios::binary);
        if (!out.is_open()) return false;

        static_assert(sizeof(asset::BudMeshHeader) == asset::MESH_HEADER_SIZE, "BudMeshHeader size mismatch!");
        static_assert(offsetof(asset::BudMeshHeader, vertex_offset) == asset::MESH_HEADER_VERTEX_OFFSET, "BudMeshHeader alignment mismatch!");
        static_assert(offsetof(asset::BudMeshHeader, submesh_count) == asset::MESH_HEADER_SUBMESH_COUNT_OFFSET, "BudMeshHeader submesh_count offset mismatch!");
        static_assert(sizeof(asset::SubMeshDescriptor) == asset::SUBMESH_DESCRIPTOR_SIZE, "SubMeshDescriptor size mismatch!");

        asset::BudMeshHeader header = {};
        header.magic = asset::MESH_MAGIC;
        header.version = asset::MESH_VERSION;
        header.total_vertices = (uint32_t)all_vertices.size();
        header.total_indices = (uint32_t)all_indices.size();
        header.meshlet_count = (uint32_t)all_meshlets.size();
        header.submesh_count = (uint32_t)submeshes.size();

        // Textures already processed at the start
        header.texture_count = (uint32_t)texture_paths.size();
        header.material_count = (uint32_t)materials.size();

        header.aabb_min[0] = header.aabb_min[1] = header.aabb_min[2] = std::numeric_limits<float>::max();
        header.aabb_max[0] = header.aabb_max[1] = header.aabb_max[2] = -std::numeric_limits<float>::max();
        for (const auto& v : all_vertices) {
            header.aabb_min[0] = std::min(header.aabb_min[0], v.position[0]);
            header.aabb_min[1] = std::min(header.aabb_min[1], v.position[1]);
            header.aabb_min[2] = std::min(header.aabb_min[2], v.position[2]);
            header.aabb_max[0] = std::max(header.aabb_max[0], v.position[0]);
            header.aabb_max[1] = std::max(header.aabb_max[1], v.position[1]);
            header.aabb_max[2] = std::max(header.aabb_max[2], v.position[2]);
        }

        size_t current_offset = sizeof(header);
        header.vertex_offset = current_offset;
        current_offset += all_vertices.size() * sizeof(asset::Vertex);
        header.index_offset = current_offset;
        current_offset += all_indices.size() * sizeof(uint32_t);
        header.meshlet_offset = current_offset;
        current_offset += all_meshlets.size() * sizeof(asset::MeshletDescriptor);
        header.vertex_index_offset = current_offset;
        current_offset += all_meshlet_vertices.size() * sizeof(uint32_t);
        header.meshlet_index_offset = current_offset;
        current_offset += all_meshlet_triangles.size() * sizeof(uint32_t);
        header.cull_data_offset = current_offset;
        current_offset += all_cull_data.size() * sizeof(asset::MeshletCullData);
        header.submesh_offset = current_offset;
        current_offset += submeshes.size() * sizeof(asset::SubMeshDescriptor);
        header.material_offset = current_offset;
        current_offset += materials.size() * sizeof(asset::MaterialDescriptor);
        header.texture_offset = current_offset;
        // Total size of all strings including null terminators
        for (const auto& path : texture_paths) {
            current_offset += path.length() + 1;
        }

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(all_vertices.data()), all_vertices.size() * sizeof(asset::Vertex));
        out.write(reinterpret_cast<const char*>(all_indices.data()), all_indices.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(all_meshlets.data()), all_meshlets.size() * sizeof(asset::MeshletDescriptor));
        out.write(reinterpret_cast<const char*>(all_meshlet_vertices.data()), all_meshlet_vertices.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(all_meshlet_triangles.data()), all_meshlet_triangles.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(all_cull_data.data()), all_cull_data.size() * sizeof(asset::MeshletCullData));
        out.write(reinterpret_cast<const char*>(submeshes.data()), submeshes.size() * sizeof(asset::SubMeshDescriptor));
        // Write material table
        if (!materials.empty()) {
            out.write(reinterpret_cast<const char*>(materials.data()), materials.size() * sizeof(asset::MaterialDescriptor));
        }

        for (const auto& path : texture_paths) {
            out.write(path.c_str(), path.length() + 1);
        }

        if (page_size > 0) {
            out.close();

            // LOD constants for page metadata (must match the instance loop).
            constexpr uint32_t LOD_COUNT = 3;
            constexpr float lod_target_errors[LOD_COUNT] = { 0.0f, 1e-3f, 5e-3f };

            std::string json_path = output_path;
            std::string bin_path = output_path;
            if (json_path.find(".budmesh") != std::string::npos) {
                json_path = json_path.substr(0, json_path.rfind('.')) + ".budmesh.json";
                bin_path  = bin_path.substr(0, bin_path.rfind('.')) + ".budmesh.bin";
            }

            // Build page table (Nanite-style: spatial BVH subtrees become pages).
            // Reserve 64B header + up to 47B alignment + 1 vertex margin per page.
            constexpr uint64_t page_reserve = 64 + 47 + 48;
            const uint32_t meshlet_total = (uint32_t)all_meshlets.size();

            // meshlet_ordering maps the spatial-BVH traversal order back to the
            // original meshlet index. The binary page data (vertices/indices/
            // descriptors) below is written page by page in this new order, so
            // page_starts/page_counts index into the REORDERED meshlet sequence.
            std::vector<uint32_t> meshlet_ordering(meshlet_total);
            for (uint32_t i = 0; i < meshlet_total; ++i) meshlet_ordering[i] = i;

            // Page model v3 (Nanite-like hierarchy):
            //   - Every BVH node becomes a page. Leaf pages keep the full
            //     LOD0..LOD2 cluster set of their subtree (existing behavior).
            //   - Internal (coarse) pages aggregate the subtree's LOD2
            //     clusters so an unloaded leaf can be replaced by its parent's
            //     coarse geometry while streaming (no holes).
            // page_cluster_ids[page] holds the original meshlet ids of that
            // page, ordered by LOD then by the BVH layout.
            std::vector<std::vector<uint32_t>> page_cluster_ids;
            std::vector<uint32_t> page_parent;       // INVALID_INDEX for root
            std::vector<std::vector<uint32_t>> page_children;
            std::vector<uint8_t> page_is_coarse;

            auto new_page = [&](std::vector<uint32_t> cluster_ids, uint32_t parent, bool coarse) -> uint32_t {
                uint32_t pid = (uint32_t)page_cluster_ids.size();
                page_cluster_ids.push_back(std::move(cluster_ids));
                page_parent.push_back(parent);
                page_children.emplace_back();
                page_is_coarse.push_back(coarse ? 1u : 0u);
                if (parent != asset::INVALID_INDEX)
                    page_children[parent].push_back(pid);
                return pid;
            };

            // meshlet_size[i] in bytes for the given original meshlet index.
            // NOTE: pages store PageClusterDesc (28 B in v3, was 24 in v2), so
            // the page-size estimation must use the v3 descriptor stride or
            // leaf pages overflow page_size and corrupt the binary layout.
            auto meshlet_size = [&](uint32_t mi) -> uint64_t {
                return all_meshlets[mi].vertex_count * sizeof(asset::Vertex)
                     + all_meshlets[mi].triangle_count * 3 * sizeof(uint32_t)
                     + asset::PAGE_CLUSTER_DESC_STRIDE + sizeof(asset::MeshletCullData);
            };
            // Total byte size of meshlets in the ordering range [begin, end).
            auto range_size = [&](const std::vector<uint32_t>& ord, size_t begin, size_t end) -> uint64_t {
                uint64_t s = 0;
                for (size_t k = begin; k < end; ++k) s += meshlet_size(ord[k]);
                return s;
            };

            // Recursively split [begin,end) of meshlet_ordering by the meshlet
            // bounding-sphere center along the largest-extent axis (median
            // split). Leaf pages hold the subtree's full LOD set; internal
            // nodes additionally become coarse pages aggregating LOD2.
            std::function<uint32_t(size_t, size_t, uint32_t)> bvh_build =
                [&](size_t begin, size_t end, uint32_t parent) -> uint32_t {
                size_t count = end - begin;
                if (count == 0) return asset::INVALID_INDEX;
                uint64_t sz = range_size(meshlet_ordering, begin, end);
                if (count == 1 || sz + page_reserve <= page_size) {
                    // Leaf page: full LOD set of this subtree.
                    // NOTE: must use iterator range construction; (begin, end)
                    // are size_t indices, and vector(count, value) would fill
                    // 'begin' copies of 'end' -> out-of-bounds all_cluster_lod.
                    std::vector<uint32_t> ids(meshlet_ordering.begin() + begin, meshlet_ordering.begin() + end);
                    // Order by LOD (LOD0..LOD2) for contiguous per-LOD ranges.
                    std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
                        return all_cluster_lod[a] < all_cluster_lod[b];
                    });
                    return new_page(std::move(ids), parent, false);
                }
                // Compute AABB over the meshlet bounding-sphere centers.
                float cmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
                float cmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (size_t k = begin; k < end; ++k) {
                    uint32_t mi = meshlet_ordering[k];
                    const float* c = all_cull_data[mi].bounding_sphere;
                    for (int a = 0; a < 3; ++a) {
                        cmin[a] = std::min(cmin[a], c[a]);
                        cmax[a] = std::max(cmax[a], c[a]);
                    }
                }
                float ext[3] = { cmax[0]-cmin[0], cmax[1]-cmin[1], cmax[2]-cmin[2] };
                int axis = (ext[0] >= ext[1] && ext[0] >= ext[2]) ? 0 : (ext[1] >= ext[2] ? 1 : 2);
                size_t mid = begin + count / 2;
                std::nth_element(meshlet_ordering.begin() + begin, meshlet_ordering.begin() + mid, meshlet_ordering.begin() + end,
                    [&](uint32_t a, uint32_t b) {
                        return all_cull_data[a].bounding_sphere[axis] < all_cull_data[b].bounding_sphere[axis];
                    });

                // Recurse children first.
                uint32_t left = bvh_build(begin, mid, asset::INVALID_INDEX);
                uint32_t right = bvh_build(mid, end, asset::INVALID_INDEX);

                // Internal node -> coarse page. Per-mesh DAG rule: a coarse
                // page must only group clusters of a SINGLE mesh (they are one
                // simplification chain). Aggregate the LOD2 clusters of the
                // mesh that has the most clusters in this subtree; other meshes'
                // LOD2 stays on their own leaf pages (no cross-mesh stitching).
                std::unordered_map<uint32_t, size_t> mesh_cluster_count;
                for (size_t k = begin; k < end; ++k) {
                    uint32_t mi = meshlet_ordering[k];
                    if (all_cluster_lod[mi] == 2)
                        mesh_cluster_count[all_cluster_mesh[mi]]++;
                }
                uint32_t dominant_mesh = asset::INVALID_INDEX;
                size_t dominant_count = 0;
                for (const auto& [mesh_idx, cnt] : mesh_cluster_count) {
                    if (cnt > dominant_count) { dominant_mesh = mesh_idx; dominant_count = cnt; }
                }
                std::vector<uint32_t> coarse_ids;
                if (dominant_mesh != asset::INVALID_INDEX) {
                    for (size_t k = begin; k < end; ++k) {
                        uint32_t mi = meshlet_ordering[k];
                        if (all_cluster_lod[mi] == 2 && all_cluster_mesh[mi] == dominant_mesh)
                            coarse_ids.push_back(mi);
                    }
                }
                // Coarse pages must fit page_size (v3 descriptor is 28 B, so a
                // large subtree's LOD2 aggregate can overflow). If it does, split
                // the coarse set recursively until each coarse page fits.
                // Returns the topmost coarse page created for this subtree.
                std::function<uint32_t(std::vector<uint32_t>, uint32_t, uint32_t)> build_coarse =
                    [&](std::vector<uint32_t> ids, uint32_t par, uint32_t top) -> uint32_t {
                    uint64_t s = 0;
                    for (uint32_t mi : ids) s += meshlet_size(mi);
                    if (ids.size() == 1 || s + page_reserve <= page_size) {
                        uint32_t pid = new_page(std::move(ids), par, true);
                        return (top == asset::INVALID_INDEX) ? pid : top;
                    }
                    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                    for (uint32_t mi : ids) {
                        const float* c = all_cull_data[mi].bounding_sphere;
                        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], c[a]); hi[a] = std::max(hi[a], c[a]); }
                    }
                    float ex[3] = { hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2] };
                    int ax = (ex[0] >= ex[1] && ex[0] >= ex[2]) ? 0 : (ex[1] >= ex[2] ? 1 : 2);
                    size_t m = ids.size() / 2;
                    std::nth_element(ids.begin(), ids.begin() + m, ids.end(),
                        [&](uint32_t a, uint32_t b) { return all_cull_data[a].bounding_sphere[ax] < all_cull_data[b].bounding_sphere[ax]; });
                    std::vector<uint32_t> la(ids.begin(), ids.begin() + m);
                    std::vector<uint32_t> ra(ids.begin() + m, ids.end());
                    // Both halves are coarse pages under `par`; keep the first
                    // created coarse page as the subtree's coarse representative.
                    uint32_t t1 = build_coarse(std::move(la), par, top);
                    uint32_t t2 = build_coarse(std::move(ra), par, t1);
                    return t2;
                };
                if (!coarse_ids.empty()) {
                    uint32_t cpid = build_coarse(std::move(coarse_ids), parent, asset::INVALID_INDEX);
                    // cpid is always a valid coarse page id now (coarse_ids was
                    // non-empty). Reparent the (leaf) children under it.
                    if (cpid == asset::INVALID_INDEX) cpid = asset::INVALID_INDEX; // defensive
                    if (left != asset::INVALID_INDEX && cpid != asset::INVALID_INDEX) { page_parent[left] = cpid; page_children[cpid].push_back(left); }
                    if (right != asset::INVALID_INDEX && cpid != asset::INVALID_INDEX) { page_parent[right] = cpid; page_children[cpid].push_back(right); }
                    return cpid == asset::INVALID_INDEX ? (left != asset::INVALID_INDEX ? left : right) : cpid;
                }
                // No LOD2 in this subtree: propagate the parent up.
                if (left != asset::INVALID_INDEX) page_parent[left] = parent;
                if (right != asset::INVALID_INDEX) page_parent[right] = parent;
                return parent == asset::INVALID_INDEX
                    ? (left != asset::INVALID_INDEX ? left : right)
                    : parent;
            };

            uint32_t root_page = bvh_build(0, meshlet_total, asset::INVALID_INDEX);

            // Per-meshlet base color texture index (from the owning submesh's material)
            std::vector<uint32_t> meshlet_tex_index(all_meshlets.size(), 0);
            for (const auto& sub : submeshes) {
                uint32_t tex = materials[sub.material_id].base_color_texture;
                for (uint32_t m = sub.meshlet_start; m < sub.meshlet_start + sub.meshlet_count; ++m)
                    meshlet_tex_index[m] = tex;
            }

            // JSON
            nlohmann::json j;
            j["magic"] = "BUDM"; j["version"] = asset::MESH_VERSION;
            j["data_uri"] = std::filesystem::path(bin_path).filename().string();
            j["pages"] = nlohmann::json::array();
            for (uint32_t i = 0; i < (uint32_t)page_cluster_ids.size(); ++i) {
                const auto& ids = page_cluster_ids[i];
                const size_t mc = ids.size();
                // Split the page into contiguous per-(LOD, material) submeshes.
                // ids are already LOD-ordered (LOD0..LOD2) so each LOD level is
                // a contiguous run and submesh ranges map 1:1 to the index data.
                nlohmann::json subs = nlohmann::json::array();
                uint32_t cum_idx = 0;      // running offset into the page index data
                uint32_t cum_clusters = 0; // running cluster offset within the page
                int cur_lod = -1;
                uint32_t cur_tex = 0;
                uint32_t run_count = 0;
                uint32_t run_clusters = 0;
                uint32_t run_start = 0;
                uint32_t run_cluster_start = 0;
                std::vector<uint32_t> lod_range_start(LOD_COUNT, 0);
                std::vector<uint32_t> lod_range_count(LOD_COUNT, 0);

                for (uint32_t k = 0; k < (uint32_t)mc; ++k) {
                    uint32_t m = ids[k]; // original meshlet index
                    uint32_t lod = all_cluster_lod[m];
                    uint32_t tex = meshlet_tex_index[m];
                    uint32_t tris = all_meshlets[m].triangle_count * 3;
                    if ((int)lod != cur_lod || tex != cur_tex) {
                        if (run_count > 0)
                            subs.push_back({{"index_start", run_start}, {"index_count", run_count},
                                            {"material_id", cur_tex}, {"lod_level", cur_lod},
                                            {"cluster_start", run_cluster_start}, {"cluster_count", run_clusters}});
                        cur_lod = (int)lod; cur_tex = tex;
                        run_start = cum_idx; run_cluster_start = cum_clusters;
                        run_count = 0; run_clusters = 0;
                    }
                    if (lod_range_count[lod] == 0)
                        lod_range_start[lod] = cum_idx; // first cluster of this LOD
                    run_count += tris;
                    run_clusters += 1;
                    cum_idx += tris;
                    cum_clusters += 1;
                    lod_range_count[lod] = cum_idx - lod_range_start[lod];
                }
                if (run_count > 0)
                    subs.push_back({{"index_start", run_start}, {"index_count", run_count},
                                    {"material_id", cur_tex}, {"lod_level", cur_lod},
                                    {"cluster_start", run_cluster_start}, {"cluster_count", run_clusters}});

                // Per-LOD index ranges [start, count] within the page index data.
                nlohmann::json lod_ranges = nlohmann::json::array();
                for (uint32_t lod = 0; lod < LOD_COUNT; ++lod)
                    lod_ranges.push_back({lod_range_start[lod], lod_range_count[lod]});

                // Per-page AABB (for distance-based on-demand streaming).
                float amin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
                float amax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (size_t m = 0; m < mc; ++m) {
                    uint32_t mi = ids[m]; // original meshlet index
                    const auto& md = all_meshlets[mi];
                    for (uint32_t v = 0; v < md.vertex_count; ++v) {
                        const auto& vt = all_vertices[all_meshlet_vertices[md.vertex_offset + v]];
                        for (int k = 0; k < 3; ++k) {
                            amin[k] = std::min(amin[k], vt.position[k]);
                            amax[k] = std::max(amax[k], vt.position[k]);
                        }
                    }
                }

                nlohmann::json children = nlohmann::json::array();
                for (uint32_t c : page_children[i]) children.push_back(c);

                j["pages"].push_back({
                    {"page_id", i},
                    {"file_offset", (uint64_t)i * page_size},
                    {"capacity", page_size},
                    {"cluster_count", (uint32_t)mc},
                    {"parent_page_id", page_parent[i] == asset::INVALID_INDEX ? nlohmann::json(nullptr) : nlohmann::json(page_parent[i])},
                    {"is_coarse", page_is_coarse[i] != 0u},
                    {"children", children},
                    {"material_id", ids.empty() ? 0u : meshlet_tex_index[ids[0]]},
                    {"aabb_min", {amin[0], amin[1], amin[2]}},
                    {"aabb_max", {amax[0], amax[1], amax[2]}},
                    {"lod_ranges", lod_ranges},
                    {"submeshes", subs}
                });
            }
            j["textures"] = nlohmann::json(texture_paths);
            std::ofstream jf(json_path); if (jf.is_open()) jf << j.dump(4);

            // Binary
            std::ofstream bf(bin_path, std::ios::binary);
            if (bf.is_open()) {
                for (uint32_t pi = 0; pi < (uint32_t)page_cluster_ids.size(); ++pi) {
                    const auto& ids = page_cluster_ids[pi];
                    const uint32_t mc = (uint32_t)ids.size();
                    uint64_t pg_start = bf.tellp();

                    float amin[3]={FLT_MAX,FLT_MAX,FLT_MAX}, amax[3]={-FLT_MAX,-FLT_MAX,-FLT_MAX};
                    uint32_t vcnt = 0;
                    for (uint32_t m = 0; m < mc; ++m) {
                        uint32_t mi = ids[m]; // original meshlet index
                        const auto& md = all_meshlets[mi];
                        vcnt += md.vertex_count;
                        for (uint32_t v=0; v<md.vertex_count; ++v) {
                            const auto& vt = all_vertices[all_meshlet_vertices[md.vertex_offset+v]];
                            for (int k=0;k<3;++k){amin[k]=std::min(amin[k],vt.position[k]);amax[k]=std::max(amax[k],vt.position[k]);}
                        }
                    }
                    uint32_t ce = asset::PAGE_HEADER_SIZE + mc*asset::PAGE_CLUSTER_DESC_STRIDE + mc*asset::PAGE_CULL_DATA_STRIDE;
                    uint32_t vo = ce, io = vo + vcnt*asset::PAGE_VERTEX_STRIDE;

                    // Build page-local vertex remap: global vertex id -> page-local vertex id
                    std::unordered_map<uint32_t,uint32_t> page_remap;
                    std::vector<uint32_t> page_vertex_ids; // page-local vertex id -> global vertex id
                    std::vector<uint32_t> meshlet_local_vert_off; // per meshlet: page-local vertex offset
                    std::vector<uint32_t> meshlet_local_tri_off;  // per meshlet: page-local triangle offset

                    uint32_t tri_total = 0;
                    for (uint32_t m=0;m<mc;++m) {
                        uint32_t mi = ids[m];
                        const auto& md = all_meshlets[mi];
                        meshlet_local_vert_off.push_back((uint32_t)page_vertex_ids.size());
                        for(uint32_t v=0;v<md.vertex_count;++v) {
                            uint32_t gvid = all_meshlet_vertices[md.vertex_offset+v];
                            if (page_remap.find(gvid) == page_remap.end()) {
                                page_remap[gvid] = (uint32_t)page_vertex_ids.size();
                                page_vertex_ids.push_back(gvid);
                            }
                        }
                        meshlet_local_tri_off.push_back(tri_total);
                        tri_total += md.triangle_count*3;
                    }

                    asset::PageBinaryHeader h={};
                    h.magic=asset::PageBinaryHeader::MAGIC; h.version=asset::PageBinaryHeader::VERSION;
                    h.cluster_count=mc; h.parent_page_id = page_parent[pi] == asset::INVALID_INDEX ? asset::INVALID_INDEX : page_parent[pi];
                    h.vertex_count=(uint32_t)page_vertex_ids.size(); h.index_count=tri_total;
                    std::memcpy(h.aabb_min,amin,sizeof(amin)); std::memcpy(h.aabb_max,amax,sizeof(amax));
                    // Align vertex data offset to 48-byte stride so draw vertexOffset stays integer-exact
                    uint32_t vo2 = (ce + asset::PAGE_VERTEX_STRIDE - 1) / asset::PAGE_VERTEX_STRIDE * asset::PAGE_VERTEX_STRIDE;
                    uint32_t io2 = vo2 + (uint32_t)page_vertex_ids.size()*asset::PAGE_VERTEX_STRIDE;
                    h.vertex_data_offset=vo2; h.index_data_offset=io2;
                    h.max_error = lod_target_errors[LOD_COUNT-1]; // coarsest level error (for threshold scale)
                    h.padding[0] = (page_is_coarse[pi] ? 1u : 0u) | (ids.empty() ? 0u : (meshlet_tex_index[ids[0]] << 1));
                    // NOTE: padding has exactly one element (byte 60..63, struct is 64 bytes);
                    // writing padding[1] would overflow past the end of the struct.
                    bf.write((const char*)&h,sizeof(h));

                    for (uint32_t m=0;m<mc;++m) {
                        uint32_t mi = ids[m];
                        const auto& md = all_meshlets[mi];
                        asset::PageClusterDesc pd = {};
                        pd.vertex_offset = meshlet_local_vert_off[m];
                        pd.vertex_count = md.vertex_count;
                        pd.triangle_offset = meshlet_local_tri_off[m];
                        pd.triangle_count = md.triangle_count;
                        pd.lod_level = all_cluster_lod[mi];
                        pd.cluster_error = all_cluster_error[mi];
                        uint32_t lod = all_cluster_lod[mi];
                        pd.parent_error = (lod + 1 < LOD_COUNT) ? lod_target_errors[lod + 1] : lod_target_errors[LOD_COUNT - 1];
                        bf.write((const char*)&pd,sizeof(pd));
                    }
                    for (uint32_t m=0;m<mc;++m) bf.write((const char*)&all_cull_data[ids[m]],sizeof(asset::MeshletCullData));
                    // Pad to aligned vertex_data_offset
                    {
                        uint64_t cur = (uint64_t)bf.tellp() - pg_start;
                        if (cur < vo2) { std::vector<char> pad((size_t)(vo2-cur),0); bf.write(pad.data(),pad.size()); }
                    }
                    for (uint32_t gvid : page_vertex_ids) bf.write((const char*)&all_vertices[gvid],sizeof(asset::Vertex));
                    for (uint32_t m=0;m<mc;++m){
                        const auto& md = all_meshlets[ids[m]];
                        for(uint32_t t=0;t<md.triangle_count*3;++t) {
                            uint32_t local_vi = all_meshlet_triangles[md.triangle_offset+t];
                            uint32_t gvid = all_meshlet_vertices[md.vertex_offset+local_vi];
                            uint32_t pvid = page_remap[gvid];
                            bf.write((const char*)&pvid,sizeof(uint32_t));
                        }
                    }
                    uint64_t wr = (uint64_t)bf.tellp()-pg_start;
                    if (wr < page_size) { std::vector<char> pad((size_t)(page_size-wr),0); bf.write(pad.data(),pad.size()); }
                }
            }
            std::cout << "[BudAssetTool] Exported " << all_meshlets.size() << " meshlets in " << page_cluster_ids.size() << " pages to " << json_path << std::endl;
            return true;
        }

        std::cout << "[BudAssetTool] Successfully exported " << header.submesh_count << " submeshes, " << header.meshlet_count << " meshlets, " << header.material_count << " materials and " << header.texture_count << " textures to " << output_path << std::endl;
        return true;
    }

} // namespace bud::tool

#include <string>

#if !defined(BUD_HAVE_SPIRV_REFLECT)
bool bud::tool::AssetProcessor::validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path, unsigned int /*max_workers*/) {
    (void)shader_dir; (void)report_path;
    std::cerr << "[BudAssetTool] SPIRV-Reflect not available in this build. Install spirv-reflect via vcpkg to enable validation." << std::endl;
    return false;
}
#else
bool bud::tool::AssetProcessor::validate_shaders_in_directory(const std::string& shader_dir, const std::string& report_path, unsigned int max_workers) {
    namespace fs = std::filesystem;
    fs::path dir(shader_dir);
    // Ensure tmp dir exists under repo for temporary compiler outputs
    fs::path tmp_dir = std::filesystem::current_path() / "tmp";
    std::error_code tmp_ec;
    fs::create_directories(tmp_dir, tmp_ec);
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cerr << "[BudAssetTool] Shader directory does not exist: " << shader_dir << std::endl;
        return false;
    }

    std::vector<std::string> exts = { ".vert", ".frag", ".comp", ".geom", ".tesc", ".tese" };
    nlohmann::json report_json;
    report_json["shaders"] = nlohmann::json::array();

    // Collect shader files
    std::vector<fs::path> shader_files;
    for (auto& p : fs::recursive_directory_iterator(dir)) {
        if (!p.is_regular_file()) continue;
        fs::path path = p.path();
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
        shader_files.push_back(path);
    }

    std::atomic<bool> all_ok{true};
    // Determine worker count: prefer explicit parameter, then env var, then hardware concurrency
    unsigned int workers = max_workers;
    if (workers == 0) {
        // check env var BUD_SHADER_WORKERS or BUD_ASSET_TOOL_WORKERS
        const char* env = std::getenv("BUD_SHADER_WORKERS");
        if (!env) env = std::getenv("BUD_ASSET_TOOL_WORKERS");
        if (env) {
            try { workers = std::stoul(env); }
            catch (...) { workers = 0; }
        }
    }
    if (workers == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        workers = hw == 0 ? 1u : hw;
    }
    unsigned int max_workers_final = workers;
    std::cout << "[BudAssetTool] Using " << max_workers_final << " parallel workers for shader validation." << std::endl;

    // Launch tasks in parallel using a simple batch/future approach
    std::vector<std::future<nlohmann::json>> futures;
    futures.reserve(shader_files.size());

    for (const auto& path : shader_files) {
        futures.push_back(std::async(std::launch::async, [path, tmp_dir]() -> nlohmann::json {
            nlohmann::json entry;
            entry["path"] = path.string();
            entry["compiled"] = false;
            entry["warnings"] = nlohmann::json::array();
            entry["errors"] = nlohmann::json::array();
            entry["bindings"] = nlohmann::json::array();
            entry["inputs"] = nlohmann::json::array();
            entry["outputs"] = nlohmann::json::array();
            entry["push_constants"] = nlohmann::json::array();

            try {
                std::cout << "[BudAssetTool] Validating shader: " << path << std::endl;
                fs::path out_spv = tmp_dir / (path.filename().string() + std::string(".spv"));
                bool compiled = compile_shader_with_glslc(path, out_spv);
                entry["compiled"] = compiled;

                fs::path log_path = tmp_dir / (path.filename().string() + std::string(".spv.log"));
                if (auto log_data = bud::tool_support::read_binary_file(log_path)) {
                    std::string compiler_output(log_data->begin(), log_data->end());
                    entry["compiler_output"] = compiler_output;
                }

                if (!compiled) {
                    std::string msg = std::string("Failed to compile shader with glslc: ") + path.string();
                    std::cerr << "[BudAssetTool] " << msg << std::endl;
                    entry["errors"].push_back(msg);
                    // cleanup
                    std::error_code ec; fs::remove(out_spv, ec); fs::remove(log_path, ec);
                    return entry;
                }

                auto spv_data_opt = bud::tool_support::read_binary_file(out_spv);
                if (!spv_data_opt) {
                    std::string msg = std::string("Failed to read compiled SPV: ") + out_spv.string();
                    std::cerr << "[BudAssetTool] " << msg << std::endl;
                    entry["errors"].push_back(msg);
                    return entry;
                }
                std::vector<char> data = *spv_data_opt;

                SpvReflectShaderModule module;
                SpvReflectResult res = spvReflectCreateShaderModule(data.size(), data.data(), &module);
                if (res != SPV_REFLECT_RESULT_SUCCESS) {
                    std::string msg = std::string("SPIRV-Reflect: failed to create module for ") + path.string();
                    std::cerr << "[BudAssetTool] " << msg << std::endl;
                    entry["errors"].push_back(msg);
                    return entry;
                }

                entry["stage"] = module.shader_stage;

                uint32_t set_count = 0;
                res = spvReflectEnumerateDescriptorSets(&module, &set_count, nullptr);
                if (res == SPV_REFLECT_RESULT_SUCCESS && set_count > 0) {
                    std::vector<SpvReflectDescriptorSet*> sets(set_count);
                    res = spvReflectEnumerateDescriptorSets(&module, &set_count, sets.data());
                    if (res == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t si = 0; si < set_count; ++si) {
                            SpvReflectDescriptorSet* set = sets[si];
                            nlohmann::json set_json;
                            set_json["set"] = set->set;
                            set_json["bindings"] = nlohmann::json::array();
                            for (uint32_t bi = 0; bi < set->binding_count; ++bi) {
                                const SpvReflectDescriptorBinding* binding = set->bindings[bi];
                                nlohmann::json b;
                                b["set"] = set->set;
                                b["binding"] = binding->binding;
                                b["descriptor_type"] = binding->descriptor_type;
                                b["array_dims"] = binding->array.dims_count > 0 ? binding->array.dims[0] : 0;
                                b["name"] = binding->name ? binding->name : "";
                                set_json["bindings"].push_back(b);
                            }
                            entry["bindings"].push_back(set_json);
                        }
                    }
                }

                uint32_t input_count = 0;
                if (spvReflectEnumerateInputVariables(&module, &input_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && input_count > 0) {
                    std::vector<SpvReflectInterfaceVariable*> inputs(input_count);
                    if (spvReflectEnumerateInputVariables(&module, &input_count, inputs.data()) == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t ii = 0; ii < input_count; ++ii) {
                            SpvReflectInterfaceVariable* v = inputs[ii];
                            nlohmann::json iv;
                            iv["location"] = v->location;
                            iv["name"] = v->name ? v->name : "";
                            iv["built_in"] = v->built_in;
                            iv["format"] = v->format;
                            entry["inputs"].push_back(iv);
                        }
                    }
                }

                uint32_t output_count = 0;
                if (spvReflectEnumerateOutputVariables(&module, &output_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && output_count > 0) {
                    std::vector<SpvReflectInterfaceVariable*> outputs(output_count);
                    if (spvReflectEnumerateOutputVariables(&module, &output_count, outputs.data()) == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t oi = 0; oi < output_count; ++oi) {
                            SpvReflectInterfaceVariable* v = outputs[oi];
                            nlohmann::json ov;
                            ov["location"] = v->location;
                            ov["name"] = v->name ? v->name : "";
                            ov["built_in"] = v->built_in;
                            ov["format"] = v->format;
                            entry["outputs"].push_back(ov);
                        }
                    }
                }

                uint32_t pcb_count = 0;
                if (spvReflectEnumeratePushConstantBlocks(&module, &pcb_count, nullptr) == SPV_REFLECT_RESULT_SUCCESS && pcb_count > 0) {
                    std::vector<SpvReflectBlockVariable*> pcbs(pcb_count);
                    if (spvReflectEnumeratePushConstantBlocks(&module, &pcb_count, pcbs.data()) == SPV_REFLECT_RESULT_SUCCESS) {
                        for (uint32_t pi = 0; pi < pcb_count; ++pi) {
                            SpvReflectBlockVariable* b = pcbs[pi];
                            nlohmann::json pjson;
                            pjson["size"] = b->size;
                            pjson["name"] = b->name ? b->name : "";
                            entry["push_constants"].push_back(pjson);
                        }
                    }
                }

                spvReflectDestroyShaderModule(&module);
                // remove temp spv
                std::error_code ec; fs::remove(out_spv, ec);
            }
            catch (const std::exception& e) {
                entry["errors"].push_back(std::string("Exception: ") + e.what());
            }
            return entry;
        }));
        // If too many outstanding futures, wait for some
        while (futures.size() > max_workers) {
            auto f = std::move(futures.front());
            futures.erase(futures.begin());
            nlohmann::json e = f.get();
            if (e.contains("compiled") && !e["compiled"].get<bool>()) all_ok.store(false);
            report_json["shaders"].push_back(e);
        }
    }

    // Collect remaining futures
    for (auto& fut : futures) {
        nlohmann::json e = fut.get();
        if (e.contains("compiled") && !e["compiled"].get<bool>()) all_ok.store(false);
        report_json["shaders"].push_back(e);
    }

    if (!report_path.empty()) {
        std::ofstream out(report_path);
        if (out.is_open()) {
            out << report_json.dump(2);
            out.close();
        } else {
            std::cerr << "[BudAssetTool] Failed to write report to " << report_path << std::endl;
        }
    }

    return all_ok.load();
}
#endif


// end of file


