#include "stl_importer.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Geometry/ConvexHullBuilder.h>

namespace bud::asset_pipeline {

using bud::asset::RawMesh;
using bud::asset::RawVertex;
using bud::asset::RawSubmesh;
using bud::asset::RawMaterial;
using bud::asset::AlphaMode;

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

const std::string default_stl_texture_path = "Content/Textures/default.png";

std::optional<RawMesh> StlImporter::import_from_file(const std::string& filepath, float scale) {
    Assimp::Importer importer;
    unsigned int flags = aiProcess_Triangulate |
                         aiProcess_DropNormals |
                         aiProcess_GenSmoothNormals |
                         aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices;

    const aiScene* scene = importer.ReadFile(filepath, flags);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::cerr << "[BudAssetPipeline] Assimp error reading STL " << filepath << ": " << importer.GetErrorString() << std::endl;
        return std::nullopt;
    }

    if (scene->mNumMeshes == 0) {
        std::cerr << "[BudAssetPipeline] No meshes found in STL: " << filepath << std::endl;
        return std::nullopt;
    }

    RawMesh raw_mesh;
    raw_mesh.source_path = filepath;
    raw_mesh.textures.push_back(default_stl_texture_path);

    // Default mechanical engineering PBR material for STL parts
    RawMaterial mat;
    mat.name = "stl_default_material";
    mat.base_color_texture_path = raw_mesh.textures[0];
    mat.base_color_factor[0] = 0.75f;
    mat.base_color_factor[1] = 0.75f;
    mat.base_color_factor[2] = 0.78f;
    mat.base_color_factor[3] = 1.0f;
    mat.metallic_factor = 0.2f;
    mat.roughness_factor = 0.5f;
    mat.alpha_mode = bud::asset::AlphaMode::Opaque;
    mat.double_sided = false;
    raw_mesh.materials.push_back(std::move(mat));

    float effective_scale = scale;
    if (effective_scale <= 0.0f) {
        float max_dim = 0.0f;
        for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
            for (unsigned int v = 0; v < scene->mMeshes[m]->mNumVertices; ++v) {
                const auto& pos = scene->mMeshes[m]->mVertices[v];
                max_dim = std::max({ max_dim, std::abs(pos.x), std::abs(pos.y), std::abs(pos.z) });
            }
        }
        if (max_dim > 50.0f) {
            // Presumed millimeters (typical for CAD/STL robot parts)
            effective_scale = 0.001f;
            std::cout << "[BudAssetPipeline] Auto-detected millimeter scale in STL (max_dim = " << max_dim << "), applying scale = 0.001 to convert to meters." << std::endl;
        } else {
            effective_scale = 1.0f;
        }
    }

    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        uint32_t vertex_base = static_cast<uint32_t>(raw_mesh.vertices.size());
        uint32_t index_base = static_cast<uint32_t>(raw_mesh.indices.size());

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            RawVertex rv{};
            rv.position[0] = mesh->mVertices[v].x * effective_scale;
            rv.position[1] = mesh->mVertices[v].y * effective_scale;
            rv.position[2] = mesh->mVertices[v].z * effective_scale;

            if (mesh->HasNormals()) {
                rv.normal[0] = mesh->mNormals[v].x;
                rv.normal[1] = mesh->mNormals[v].y;
                rv.normal[2] = mesh->mNormals[v].z;
            } else {
                rv.normal[0] = 0.0f;
                rv.normal[1] = 1.0f;
                rv.normal[2] = 0.0f;
            }

            if (mesh->HasTextureCoords(0)) {
                rv.uv[0] = mesh->mTextureCoords[0][v].x;
                rv.uv[1] = mesh->mTextureCoords[0][v].y;
            } else {
                // Cylindrical/spherical UV projection fallback for untextured STL
                rv.uv[0] = 0.5f + std::atan2(rv.position[2], rv.position[0]) / (2.0f * 3.14159265f);
                rv.uv[1] = rv.position[1];
            }

            if (mesh->HasTangentsAndBitangents()) {
                rv.tangent[0] = mesh->mTangents[v].x;
                rv.tangent[1] = mesh->mTangents[v].y;
                rv.tangent[2] = mesh->mTangents[v].z;
                rv.tangent[3] = 1.0f;
            } else {
                rv.tangent[0] = 1.0f;
                rv.tangent[1] = 0.0f;
                rv.tangent[2] = 0.0f;
                rv.tangent[3] = 1.0f;
            }

            raw_mesh.vertices.push_back(rv);
        }

        uint32_t indices_added = 0;
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices == 3) {
                raw_mesh.indices.push_back(vertex_base + face.mIndices[0]);
                raw_mesh.indices.push_back(vertex_base + face.mIndices[1]);
                raw_mesh.indices.push_back(vertex_base + face.mIndices[2]);
                indices_added += 3;
            }
        }

        RawSubmesh submesh;
        submesh.name = mesh->mName.length > 0 ? mesh->mName.C_Str() : ("stl_submesh_" + std::to_string(mi));
        submesh.index_offset = index_base;
        submesh.index_count = indices_added;
        submesh.material_index = 0;
        raw_mesh.submeshes.push_back(submesh);
    }

    raw_mesh.compute_bounds();
    return raw_mesh;
}

std::optional<bud::robot::ConvexHullData> StlImporter::extract_collision_geometry(const std::string& filepath, float scale, int max_vertices) {
    ensure_jolt_initialized();

    Assimp::Importer importer;
    unsigned int flags = aiProcess_Triangulate |
                         aiProcess_JoinIdenticalVertices;

    const aiScene* scene = importer.ReadFile(filepath, flags);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::cerr << "[BudAssetPipeline] Assimp error reading STL collision " << filepath << ": " << importer.GetErrorString() << std::endl;
        return std::nullopt;
    }

    if (scene->mNumMeshes == 0) {
        std::cerr << "[BudAssetPipeline] No meshes found in STL: " << filepath << std::endl;
        return std::nullopt;
    }

    float effective_scale = scale;
    if (effective_scale <= 0.0f) {
        float max_dim = 0.0f;
        for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
            for (unsigned int v = 0; v < scene->mMeshes[m]->mNumVertices; ++v) {
                const auto& pos = scene->mMeshes[m]->mVertices[v];
                max_dim = std::max({ max_dim, std::abs(pos.x), std::abs(pos.y), std::abs(pos.z) });
            }
        }
        if (max_dim > 50.0f)
            effective_scale = 0.001f;
        else
            effective_scale = 1.0f;
    }

    JPH::Array<JPH::Vec3> in_positions;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            float px = mesh->mVertices[v].x * effective_scale;
            float py = mesh->mVertices[v].y * effective_scale;
            float pz = mesh->mVertices[v].z * effective_scale;
            in_positions.push_back(JPH::Vec3(px, py, pz));
        }
    }

    if (in_positions.empty())
        return std::nullopt;

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
    } else {
        std::cerr << "[BudAssetPipeline] Warning: ConvexHullBuilder for " << filepath 
                  << " returned status " << static_cast<int>(result) << ", using points directly." << std::endl;
        for (const auto& p : in_positions) {
            hull_data.points.push_back(p.GetX());
            hull_data.points.push_back(p.GetY());
            hull_data.points.push_back(p.GetZ());

            hull_data.aabb_min[0] = std::min(hull_data.aabb_min[0], p.GetX());
            hull_data.aabb_min[1] = std::min(hull_data.aabb_min[1], p.GetY());
            hull_data.aabb_min[2] = std::min(hull_data.aabb_min[2], p.GetZ());

            hull_data.aabb_max[0] = std::max(hull_data.aabb_max[0], p.GetX());
            hull_data.aabb_max[1] = std::max(hull_data.aabb_max[1], p.GetY());
            hull_data.aabb_max[2] = std::max(hull_data.aabb_max[2], p.GetZ());
        }
    }

    return hull_data;
}

} // namespace bud::asset_pipeline
