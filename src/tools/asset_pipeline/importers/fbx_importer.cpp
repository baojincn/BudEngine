#include "fbx_importer.hpp"
#include "texture_importer.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <functional>
#include <algorithm>
#include <filesystem>

namespace bud::asset_pipeline {

std::optional<RawMesh> FbxImporter::import_from_file(const std::string& filepath, float scale) {
    Assimp::Importer importer;
    unsigned int flags = aiProcess_Triangulate |
                         aiProcess_GenSmoothNormals |
                         aiProcess_CalcTangentSpace |
                         aiProcess_JoinIdenticalVertices;
    const aiScene* scene = importer.ReadFile(filepath, flags);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::cerr << "[BudAssetPipeline] Assimp error reading " << filepath << ": " << importer.GetErrorString() << std::endl;
        return std::nullopt;
    }

    if (scene->mNumMeshes == 0) {
        std::cerr << "[BudAssetPipeline] No meshes found in: " << filepath << std::endl;
        return std::nullopt;
    }

    RawMesh raw_mesh;
    raw_mesh.source_path = filepath;

    std::string base_dir;
    size_t last_slash = filepath.find_last_of("\\/");
    if (last_slash != std::string::npos)
        base_dir = filepath.substr(0, last_slash + 1);

    raw_mesh.textures.push_back("data/textures/default.png");

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

        std::filesystem::path tp(raw_tex_path);
        std::string fname = tp.filename().string();
        std::string stem = tp.stem().string();
        std::string lower_stem = stem;
        std::transform(lower_stem.begin(), lower_stem.end(), lower_stem.begin(), ::tolower);

        if (std::filesystem::exists(raw_tex_path))
            return raw_tex_path;
        if (!base_dir.empty() && std::filesystem::exists(base_dir + raw_tex_path))
            return base_dir + raw_tex_path;

        if (auto it = texture_files_map.find(fname); it != texture_files_map.end())
            return it->second;
        if (auto it = texture_files_map.find(lower_stem); it != texture_files_map.end())
            return it->second;

        return "";
    };

    auto find_texture_by_keyword = [&](const std::string& mat_name, const std::string& suffix) -> std::string {
        if (mat_name.empty()) return "";
        std::string lower_mat = mat_name;
        std::transform(lower_mat.begin(), lower_mat.end(), lower_mat.begin(), ::tolower);
        std::string clean_name = lower_mat;
        if (clean_name.starts_with("m_")) clean_name = clean_name.substr(2);
        
        // 1. Try clean_name with suffix
        for (const auto& [stem, full_path] : texture_files_map) {
            if (stem.find(clean_name) != std::string::npos && stem.find(suffix) != std::string::npos) {
                return full_path;
            }
        }
        
        // 2. Try prefix before trailing numbers/inst
        std::string base_word = clean_name;
        size_t under = base_word.rfind('_');
        if (under != std::string::npos && under > 2) {
            std::string sub = base_word.substr(0, under);
            for (const auto& [stem, full_path] : texture_files_map) {
                if (stem.find(sub) != std::string::npos && stem.find(suffix) != std::string::npos) {
                    return full_path;
                }
            }
        }

        // 3. Try primary word
        size_t first_under = clean_name.find('_');
        if (first_under != std::string::npos && first_under > 2) {
            std::string root = clean_name.substr(0, first_under);
            for (const auto& [stem, full_path] : texture_files_map) {
                if (stem.find(root) != std::string::npos && stem.find(suffix) != std::string::npos) {
                    return full_path;
                }
            }
        }
        return "";
    };

    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        RawMaterial rm;
        aiString name;
        if (mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
            rm.name = name.C_Str();

        aiString tex_path;
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_BASE_COLOR, 0, &tex_path) == AI_SUCCESS) {
            std::string resolved = resolve_texture_file(tex_path.C_Str());
            if (!resolved.empty()) {
                rm.base_color_texture_path = resolved;
                raw_mesh.textures.push_back(resolved);
            } else {
                std::string p = tex_path.C_Str();
                if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0)
                    p = base_dir + p;
                rm.base_color_texture_path = p;
                raw_mesh.textures.push_back(p);
            }
        } else {
            // Check all other texture slots as fallback
            for (unsigned int tt = 1; tt <= 18; ++tt) {
                if (mat->GetTexture(static_cast<aiTextureType>(tt), 0, &tex_path) == AI_SUCCESS) {
                    std::string resolved = resolve_texture_file(tex_path.C_Str());
                    if (!resolved.empty()) {
                        rm.base_color_texture_path = resolved;
                        raw_mesh.textures.push_back(resolved);
                        break;
                    }
                }
            }

            if (rm.base_color_texture_path.empty()) {
                std::string fuzzy_d = find_texture_by_keyword(rm.name, "_d");
                if (!fuzzy_d.empty()) {
                    rm.base_color_texture_path = fuzzy_d;
                    raw_mesh.textures.push_back(fuzzy_d);
                } else {
                    rm.base_color_texture_path = raw_mesh.textures[0];
                }
            }
        }

        // 1. PBR Textures from Assimp
        aiString norm_path;
        if (mat->GetTexture(aiTextureType_NORMALS, 0, &norm_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_HEIGHT, 0, &norm_path) == AI_SUCCESS) {
            rm.normal_texture_path = resolve_texture_file(norm_path.C_Str());
        }
        if (rm.normal_texture_path.empty()) {
            rm.normal_texture_path = find_texture_by_keyword(rm.name, "_n");
        }

        aiString rough_path;
        if (mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &rough_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_SHININESS, 0, &rough_path) == AI_SUCCESS) {
            rm.metallic_roughness_texture_path = resolve_texture_file(rough_path.C_Str());
        }
        if (rm.metallic_roughness_texture_path.empty()) {
            rm.metallic_roughness_texture_path = find_texture_by_keyword(rm.name, "_r");
        }

        aiString metal_path;
        if (mat->GetTexture(aiTextureType_METALNESS, 0, &metal_path) == AI_SUCCESS) {
            if (rm.metallic_roughness_texture_path.empty()) {
                rm.metallic_roughness_texture_path = resolve_texture_file(metal_path.C_Str());
            }
        }

        aiString emissive_path;
        if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &emissive_path) == AI_SUCCESS) {
            rm.emissive_texture_path = resolve_texture_file(emissive_path.C_Str());
        }
        if (rm.emissive_texture_path.empty()) {
            rm.emissive_texture_path = find_texture_by_keyword(rm.name, "_e");
        }

        // 2. Auto-scan companion PBR textures (_N, _R, _M, _E)
        if (!rm.base_color_texture_path.empty() && rm.base_color_texture_path != raw_mesh.textures[0]) {
            auto companions = TextureImporter::find_companion_pbr_textures(rm.base_color_texture_path);
            if (rm.normal_texture_path.empty()) rm.normal_texture_path = companions.normal_path;
            if (rm.metallic_roughness_texture_path.empty()) {
                if (!companions.roughness_path.empty()) rm.metallic_roughness_texture_path = companions.roughness_path;
                else if (!companions.metallic_path.empty()) rm.metallic_roughness_texture_path = companions.metallic_path;
            }
            if (rm.emissive_texture_path.empty()) rm.emissive_texture_path = companions.emissive_path;
        }

        // 3. PBR Factors
        float metallic_factor = 0.0f;
        if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic_factor) == AI_SUCCESS) {
            rm.metallic_factor = metallic_factor;
        }
        float roughness_factor = 0.5f;
        if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness_factor) == AI_SUCCESS) {
            rm.roughness_factor = roughness_factor;
        }

        rm.alpha_mode = bud::asset::AlphaMode::Opaque;
        rm.double_sided = false;
        rm.alpha_cutoff = 0.5f;

        // 4. Assimp material properties (Opacity & TwoSided)
        float opacity = 1.0f;
        if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
            if (opacity < 0.99f) {
                rm.alpha_mode = bud::asset::AlphaMode::Mask;
                rm.alpha_cutoff = 0.5f;
                rm.double_sided = true;
            }
        }
        int two_sided = 0;
        if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS) {
            if (two_sided != 0)
                rm.double_sided = true;
        }

        // 5. Texture Pixel Alpha Analysis & Companion Mask
        if (!rm.base_color_texture_path.empty() && rm.base_color_texture_path != raw_mesh.textures[0]) {
            auto alpha_info = TextureImporter::analyze_alpha(rm.base_color_texture_path);
            if (alpha_info.has_alpha) {
                rm.alpha_mode = alpha_info.alpha_mode;
                rm.alpha_cutoff = alpha_info.alpha_cutoff;
                if (rm.alpha_mode == bud::asset::AlphaMode::Mask) {
                    rm.double_sided = true;
                }
            }
        }

        raw_mesh.materials.push_back(std::move(rm));
    }

    if (raw_mesh.materials.empty()) {
        RawMaterial def_mat;
        def_mat.name = "default_material";
        def_mat.base_color_texture_path = raw_mesh.textures[0];
        raw_mesh.materials.push_back(std::move(def_mat));
    }

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
        // Auto-detect unit from FBX metadata or bounding box
        double unit_scale_factor = 1.0;
        if (scene->mMetaData && scene->mMetaData->Get("UnitScaleFactor", unit_scale_factor)) {
            if (unit_scale_factor >= 99.0 && unit_scale_factor <= 101.0) {
                effective_scale = 0.01f; // Centimeters to meters
            } else if (unit_scale_factor >= 2.5 && unit_scale_factor <= 2.6) {
                effective_scale = 0.0254f; // Inches to meters
            } else if (unit_scale_factor > 0.0) {
                effective_scale = static_cast<float>(1.0 / unit_scale_factor);
            } else {
                effective_scale = 1.0f;
            }
            std::cout << "[BudAssetPipeline] FBX UnitScaleFactor = " << unit_scale_factor << ", resolved scale = " << effective_scale << std::endl;
        } else {
            float max_dim = 0.0f;
            for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
                for (unsigned int v = 0; v < scene->mMeshes[m]->mNumVertices; ++v) {
                    const auto& pos = scene->mMeshes[m]->mVertices[v];
                    max_dim = std::max({ max_dim, std::abs(pos.x), std::abs(pos.y), std::abs(pos.z) });
                }
            }
            if (max_dim > 100.0f) {
                effective_scale = 0.01f; // Presumed centimeters
                std::cout << "[BudAssetPipeline] Auto-detected centimeter scale in FBX (max_dim = " << max_dim << "), applying scale = 0.01 to convert to meters." << std::endl;
            } else {
                effective_scale = 1.0f;
            }
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

} // namespace bud::asset_pipeline
