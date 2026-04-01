#include "bud.asset.processor.hpp"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <limits>
#include <map>
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
                                                 size_t max_vertices, size_t max_triangles, float cone_weight) {
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
            md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::OPAQUE);
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
                        md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::MASK);
                    } else {
                        md.alpha_mode = static_cast<uint8_t>(asset::AlphaMode::BLEND);
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

            size_t max_meshlets = meshopt_buildMeshletsBound(optimized_indices.size(), max_vertices, max_triangles);
            std::vector<meshopt_Meshlet> local_meshlets(max_meshlets);
            std::vector<unsigned int> local_meshlet_vertices(max_meshlets * max_vertices);
            std::vector<unsigned char> local_meshlet_triangles(max_meshlets * max_triangles * 3);

            size_t meshlet_count = meshopt_buildMeshlets(local_meshlets.data(), local_meshlet_vertices.data(), local_meshlet_triangles.data(),
                                                         optimized_indices.data(), optimized_indices.size(), &group_vertices[0].position[0], group_vertices.size(), sizeof(asset::Vertex),
                                                         max_vertices, max_triangles, cone_weight);

            local_meshlets.resize(meshlet_count);

            asset::SubMeshDescriptor sub_desc = {};
            sub_desc.index_start = group_base_index;
            sub_desc.index_count = (uint32_t)group_indices.size();
            sub_desc.meshlet_start = group_base_meshlet;
            sub_desc.meshlet_count = (uint32_t)meshlet_count;
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

            for (size_t i = 0; i < meshlet_count; ++i) {
                meshopt_Meshlet& m = local_meshlets[i];
                meshopt_optimizeMeshlet(&local_meshlet_vertices[m.vertex_offset], &local_meshlet_triangles[m.triangle_offset], m.triangle_count, m.vertex_count);

                asset::MeshletDescriptor desc = {};
                desc.vertex_offset = (uint32_t)all_meshlet_vertices.size();
                desc.vertex_count = m.vertex_count;
                desc.triangle_offset = (uint32_t)all_meshlet_triangles.size();
                desc.triangle_count = m.triangle_count;
                all_meshlets.push_back(desc);

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

