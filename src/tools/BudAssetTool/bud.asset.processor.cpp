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
#include "src/core/bud.core.hpp"

namespace bud::tool {

    bool AssetProcessor::process_gltf_to_budmesh(const std::string& input_path, const std::string& output_path) {
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

        const size_t max_vertices = 64;
        const size_t max_triangles = 128;
        const float cone_weight = 0.5f;

        std::string input_path_str = std::string(input_path);
        std::string base_dir = "";
        size_t last_slash = input_path_str.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            base_dir = input_path_str.substr(0, last_slash + 1);
        }

        std::vector<std::string> texture_paths;
        std::map<unsigned int, uint32_t> mat_to_tex_idx;
        
        uint32_t default_tex_idx = 0;
        texture_paths.push_back("data/textures/default.png");

        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            aiMaterial* mat = scene->mMaterials[i];
            aiString tex_path;
            if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) == AI_SUCCESS) {
                std::string p = tex_path.C_Str();
                if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) {
                    p = base_dir + p;
                }
                mat_to_tex_idx[i] = (uint32_t)texture_paths.size();
                texture_paths.push_back(p);
            } else {
                mat_to_tex_idx[i] = default_tex_idx;
            }
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
            uint32_t mapped_tex_idx = mat_to_tex_idx[mat_idx];

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
            sub_desc.material_id = mapped_tex_idx;
            
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
        header.version = 3;
        header.total_vertices = (uint32_t)all_vertices.size();
        header.total_indices = (uint32_t)all_indices.size();
        header.meshlet_count = (uint32_t)all_meshlets.size();
        header.submesh_count = (uint32_t)submeshes.size();

        // Textures already processed at the start
        header.texture_count = (uint32_t)texture_paths.size();

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
        
        for (const auto& path : texture_paths) {
            out.write(path.c_str(), path.length() + 1);
        }

        bud::print("[BudAssetTool] Successfully exported {} submeshes, {} meshlets, and {} textures to {}", 
            header.submesh_count, header.meshlet_count, header.texture_count, output_path);
        return true;
    }

} // namespace bud::tool
