#include "dae_importer.hpp"
#include "texture_importer.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <functional>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace bud::asset_pipeline {

const std::string default_dae_texture_path = "Content/Textures/default.png";

static void extract_dae_materials_and_textures(
    const aiScene* scene,
    const std::string& filepath,
    const std::string& base_dir,
    std::vector<RawMaterial>& out_materials,
    std::vector<std::string>& out_textures
) {
    out_textures.clear();
    out_materials.clear();

    out_textures.push_back(default_dae_texture_path);

    std::unordered_map<std::string, std::string> texture_files_map;
    if (!base_dir.empty() && std::filesystem::exists(base_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(base_dir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp") {
                    std::string s = entry.path().stem().string();
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    texture_files_map[s] = entry.path().generic_string();
                    texture_files_map[entry.path().filename().string()] = entry.path().generic_string();
                }
            }
        }
    }

    auto resolve_texture_file = [&](const std::string& raw_tex_path) -> std::string {
        if (raw_tex_path.empty())
            return "";

        std::string clean_path = raw_tex_path;
        std::replace(clean_path.begin(), clean_path.end(), '\\', '/');

        std::filesystem::path tp(clean_path);
        if (tp.is_absolute() && std::filesystem::exists(tp))
            return tp.generic_string();

        std::filesystem::path direct_rel = std::filesystem::path(base_dir) / tp;
        if (std::filesystem::exists(direct_rel))
            return direct_rel.generic_string();

        std::string filename = tp.filename().string();
        auto it_fn = texture_files_map.find(filename);
        if (it_fn != texture_files_map.end())
            return it_fn->second;

        std::string stem = tp.stem().string();
        std::string stem_lower = stem;
        std::transform(stem_lower.begin(), stem_lower.end(), stem_lower.begin(), ::tolower);
        auto it_stem = texture_files_map.find(stem_lower);
        if (it_stem != texture_files_map.end())
            return it_stem->second;

        return direct_rel.generic_string();
    };

    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        RawMaterial rm;
        aiString name;
        if (mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
            rm.name = name.C_Str();

        aiColor4D diff_col(1.0f, 1.0f, 1.0f, 1.0f);
        if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, diff_col) == AI_SUCCESS ||
            mat->Get(AI_MATKEY_BASE_COLOR, diff_col) == AI_SUCCESS) {
            rm.base_color_factor[0] = diff_col.r;
            rm.base_color_factor[1] = diff_col.g;
            rm.base_color_factor[2] = diff_col.b;
            rm.base_color_factor[3] = diff_col.a;
        }

        aiString tex_path;
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_BASE_COLOR, 0, &tex_path) == AI_SUCCESS) {
            std::string resolved = resolve_texture_file(tex_path.C_Str());
            rm.base_color_texture_path = resolved;
            out_textures.push_back(resolved);
        } else {
            rm.base_color_texture_path = out_textures[0];
        }

        aiString norm_path;
        if (mat->GetTexture(aiTextureType_NORMALS, 0, &norm_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_HEIGHT, 0, &norm_path) == AI_SUCCESS) {
            rm.normal_texture_path = resolve_texture_file(norm_path.C_Str());
        }

        aiString rough_path;
        if (mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &rough_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_SHININESS, 0, &rough_path) == AI_SUCCESS) {
            rm.metallic_roughness_texture_path = resolve_texture_file(rough_path.C_Str());
        }

        aiString metal_path;
        if (mat->GetTexture(aiTextureType_METALNESS, 0, &metal_path) == AI_SUCCESS) {
            std::string resolved = resolve_texture_file(metal_path.C_Str());
            if (rm.metallic_roughness_texture_path.empty())
                rm.metallic_roughness_texture_path = resolved;
        }

        aiString emissive_path;
        if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &emissive_path) == AI_SUCCESS) {
            rm.emissive_texture_path = resolve_texture_file(emissive_path.C_Str());
        }

        // Companion PBR scan
        if (!rm.base_color_texture_path.empty() && rm.base_color_texture_path != out_textures[0]) {
            auto companions = TextureImporter::find_companion_pbr_textures(rm.base_color_texture_path);
            if (rm.normal_texture_path.empty())
                rm.normal_texture_path = companions.normal_path;
            if (rm.metallic_roughness_texture_path.empty()) {
                if (!companions.roughness_path.empty())
                    rm.metallic_roughness_texture_path = companions.roughness_path;
                else if (!companions.metallic_path.empty())
                    rm.metallic_roughness_texture_path = companions.metallic_path;
            }
            if (rm.emissive_texture_path.empty())
                rm.emissive_texture_path = companions.emissive_path;
        }

        float metallic_factor = 0.0f;
        if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic_factor) == AI_SUCCESS)
            rm.metallic_factor = metallic_factor;

        float roughness_factor = 0.5f;
        float shininess = 0.0f;
        if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness_factor) == AI_SUCCESS) {
            rm.roughness_factor = roughness_factor;
        } else if (mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f) {
            rm.roughness_factor = std::clamp(1.0f - std::sqrt(shininess / 1000.0f), 0.05f, 1.0f);
        } else {
            rm.roughness_factor = 0.6f;
        }

        rm.alpha_mode = bud::asset::AlphaMode::Opaque;
        rm.double_sided = false;
        rm.alpha_cutoff = 0.5f;

        bool has_texture_alpha = false;
        if (!rm.base_color_texture_path.empty() && rm.base_color_texture_path != out_textures[0]) {
            auto alpha_info = TextureImporter::analyze_alpha(rm.base_color_texture_path);
            if (alpha_info.has_alpha) {
                has_texture_alpha = true;
                rm.alpha_mode = alpha_info.alpha_mode;
                rm.alpha_cutoff = alpha_info.alpha_cutoff;
                if (rm.alpha_mode == bud::asset::AlphaMode::Mask)
                    rm.double_sided = true;
            }
        }

        if (!has_texture_alpha) {
            float opacity = 1.0f;
            if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.99f && opacity > 0.001f) {
                rm.alpha_mode = bud::asset::AlphaMode::Blend;
                rm.base_color_factor[3] = opacity;
            }
            float transparency = 0.0f;
            if (mat->Get(AI_MATKEY_TRANSPARENCYFACTOR, transparency) == AI_SUCCESS && transparency > 0.01f) {
                rm.alpha_mode = bud::asset::AlphaMode::Blend;
                rm.base_color_factor[3] = 1.0f - transparency;
            }
            if (diff_col.a < 0.99f && diff_col.a > 0.001f) {
                rm.alpha_mode = bud::asset::AlphaMode::Blend;
                rm.base_color_factor[3] = diff_col.a;
            }
        }

        int two_sided = 0;
        if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS && two_sided != 0)
            rm.double_sided = true;

        out_materials.push_back(std::move(rm));
    }

    if (out_materials.empty()) {
        RawMaterial def_mat;
        def_mat.name = "default_dae_material";
        def_mat.base_color_texture_path = out_textures[0];
        out_materials.push_back(std::move(def_mat));
    }
}

std::optional<RawMesh> DaeImporter::import_from_file(const std::string& filepath, float scale) {
    Assimp::Importer importer;
    unsigned int flags = aiProcess_Triangulate |
                         aiProcess_GenSmoothNormals |
                         aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices;

    const aiScene* scene = importer.ReadFile(filepath, flags);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::cerr << "[BudAssetPipeline] Assimp error reading DAE " << filepath << ": " << importer.GetErrorString() << std::endl;
        return std::nullopt;
    }

    if (scene->mNumMeshes == 0) {
        std::cerr << "[BudAssetPipeline] No meshes found in DAE: " << filepath << std::endl;
        return std::nullopt;
    }

    RawMesh raw_mesh;
    raw_mesh.source_path = filepath;

    std::string base_dir;
    size_t last_slash = filepath.find_last_of("\\/");
    if (last_slash != std::string::npos)
        base_dir = filepath.substr(0, last_slash + 1);

    extract_dae_materials_and_textures(scene, filepath, base_dir, raw_mesh.materials, raw_mesh.textures);

    struct NodeInstance {
        unsigned int mesh_index;
        aiMatrix4x4 transform;
    };
    std::vector<NodeInstance> instances;

    std::function<void(aiNode*, aiMatrix4x4)> collect_instances = [&](aiNode* node, aiMatrix4x4 parent_xf) {
        aiMatrix4x4 cur_xf = parent_xf * node->mTransformation;
        for (unsigned int i = 0; i < node->mNumMeshes; ++i)
            instances.push_back({ node->mMeshes[i], cur_xf });
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
            collect_instances(node->mChildren[i], cur_xf);
    };
    collect_instances(scene->mRootNode, aiMatrix4x4());

    float effective_scale = scale;
    if (effective_scale <= 0.0f) {
        float max_dim = 0.0f;
        for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
            for (unsigned int v = 0; v < scene->mMeshes[m]->mNumVertices; ++v) {
                const auto& pos = scene->mMeshes[m]->mVertices[v];
                max_dim = std::max({ max_dim, std::abs(pos.x), std::abs(pos.y), std::abs(pos.z) });
            }
        }
        if (max_dim > 100.0f) {
            effective_scale = 0.01f; // Presumed centimeters
            std::cout << "[BudAssetPipeline] Auto-detected centimeter scale in DAE (max_dim = " << max_dim << "), applying scale = 0.01 to convert to meters." << std::endl;
        } else {
            effective_scale = 1.0f;
        }
    }

    for (const auto& inst : instances) {
        const aiMesh* mesh = scene->mMeshes[inst.mesh_index];
        uint32_t vertex_base = static_cast<uint32_t>(raw_mesh.vertices.size());
        uint32_t index_base = static_cast<uint32_t>(raw_mesh.indices.size());

        aiMatrix3x3 normal_matrix(inst.transform);
        normal_matrix.Inverse().Transpose();

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            RawVertex rv{};
            aiVector3D pos = inst.transform * mesh->mVertices[v];
            rv.position[0] = pos.x * effective_scale;
            rv.position[1] = pos.y * effective_scale;
            rv.position[2] = pos.z * effective_scale;

            if (mesh->HasNormals()) {
                aiVector3D n = normal_matrix * mesh->mNormals[v];
                n.Normalize();
                rv.normal[0] = n.x;
                rv.normal[1] = n.y;
                rv.normal[2] = n.z;
            } else {
                rv.normal[0] = 0.0f;
                rv.normal[1] = 1.0f;
                rv.normal[2] = 0.0f;
            }

            if (mesh->HasTextureCoords(0)) {
                rv.uv[0] = mesh->mTextureCoords[0][v].x;
                rv.uv[1] = mesh->mTextureCoords[0][v].y;
            } else {
                rv.uv[0] = 0.0f;
                rv.uv[1] = 0.0f;
            }

            if (mesh->HasTangentsAndBitangents()) {
                aiVector3D t = normal_matrix * mesh->mTangents[v];
                t.Normalize();
                rv.tangent[0] = t.x;
                rv.tangent[1] = t.y;
                rv.tangent[2] = t.z;
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
        submesh.name = mesh->mName.C_Str();
        submesh.index_offset = index_base;
        submesh.index_count = indices_added;
        submesh.material_index = std::min<uint32_t>(mesh->mMaterialIndex, static_cast<uint32_t>(raw_mesh.materials.size() - 1));
        raw_mesh.submeshes.push_back(submesh);
    }

    raw_mesh.compute_bounds();
    return raw_mesh;
}

std::optional<RawScene> DaeImporter::import_scene_from_file(const std::string& filepath, float scale) {
    Assimp::Importer importer;
    unsigned int flags = aiProcess_Triangulate |
                         aiProcess_GenSmoothNormals |
                         aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices;

    const aiScene* scene = importer.ReadFile(filepath, flags);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::cerr << "[BudAssetPipeline] Assimp error reading DAE scene " << filepath << ": " << importer.GetErrorString() << std::endl;
        return std::nullopt;
    }

    if (scene->mNumMeshes == 0) {
        std::cerr << "[BudAssetPipeline] No meshes found in DAE: " << filepath << std::endl;
        return std::nullopt;
    }

    RawScene raw_scene;
    raw_scene.name = std::filesystem::path(filepath).stem().string();
    raw_scene.source_path = filepath;

    std::string base_dir;
    size_t last_slash = filepath.find_last_of("\\/");
    if (last_slash != std::string::npos)
        base_dir = filepath.substr(0, last_slash + 1);

    extract_dae_materials_and_textures(scene, filepath, base_dir, raw_scene.materials, raw_scene.textures);

    float effective_scale = scale > 0.0f ? scale : 1.0f;

    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        RawSceneMesh sm;
        sm.mesh_index = mi;
        std::string raw_name = mesh->mName.length > 0 ? mesh->mName.C_Str() : "submesh";
        sm.name = "mesh_" + std::to_string(mi) + "_" + raw_name;

        uint32_t mat_idx = mesh->mMaterialIndex < raw_scene.materials.size() ? mesh->mMaterialIndex : 0;
        const auto& mat = raw_scene.materials[mat_idx];
        sm.is_translucent = (mat.alpha_mode == bud::asset::AlphaMode::Blend);

        sm.mesh.source_path = filepath + "#" + sm.name;
        sm.mesh.materials = raw_scene.materials;
        sm.mesh.textures = raw_scene.textures;

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
                rv.uv[0] = 0.0f;
                rv.uv[1] = 0.0f;
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

            sm.mesh.vertices.push_back(rv);
        }

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices == 3) {
                sm.mesh.indices.push_back(face.mIndices[0]);
                sm.mesh.indices.push_back(face.mIndices[1]);
                sm.mesh.indices.push_back(face.mIndices[2]);
            }
        }

        RawSubmesh submesh{};
        submesh.name = sm.name;
        submesh.index_offset = 0;
        submesh.index_count = static_cast<uint32_t>(sm.mesh.indices.size());
        submesh.material_index = mat_idx;
        sm.mesh.submeshes.push_back(submesh);
        sm.mesh.compute_bounds();

        raw_scene.meshes.push_back(std::move(sm));
    }

    std::function<void(aiNode*, aiMatrix4x4)> collect_scene_instances = [&](aiNode* node, aiMatrix4x4 parent_xf) {
        aiMatrix4x4 cur_xf = parent_xf * node->mTransformation;
        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            unsigned int mi = node->mMeshes[i];
            if (mi < raw_scene.meshes.size()) {
                RawSceneInstance inst;
                inst.name = node->mName.length > 0 ? (std::string(node->mName.C_Str()) + "_" + std::to_string(i)) : ("instance_" + std::to_string(raw_scene.instances.size()));
                inst.mesh_index = mi;
                inst.mesh_name = raw_scene.meshes[mi].name;
                inst.relative_dir = raw_scene.meshes[mi].relative_dir;
                inst.is_translucent = raw_scene.meshes[mi].is_translucent;

                inst.transform[0] = cur_xf.a1; inst.transform[1] = cur_xf.b1; inst.transform[2] = cur_xf.c1; inst.transform[3] = cur_xf.d1;
                inst.transform[4] = cur_xf.a2; inst.transform[5] = cur_xf.b2; inst.transform[6] = cur_xf.c2; inst.transform[7] = cur_xf.d2;
                inst.transform[8] = cur_xf.a3; inst.transform[9] = cur_xf.b3; inst.transform[10] = cur_xf.c3; inst.transform[11] = cur_xf.d3;
                inst.transform[12] = cur_xf.a4; inst.transform[13] = cur_xf.b4; inst.transform[14] = cur_xf.c4; inst.transform[15] = cur_xf.d4;

                raw_scene.instances.push_back(inst);
            }
        }
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
            collect_scene_instances(node->mChildren[i], cur_xf);
    };
    collect_scene_instances(scene->mRootNode, aiMatrix4x4());

    raw_scene.compute_bounds();
    return raw_scene;
}

} // namespace bud::asset_pipeline
