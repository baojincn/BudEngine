#include "gltf_importer.hpp"
#include "texture_importer.hpp"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <functional>
#include <algorithm>

namespace bud::asset_pipeline {

std::optional<RawMesh> GltfImporter::import_from_file(const std::string& filepath) {
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

    // Parse glTF JSON directly for full extension support (e.g. KHR_materials_pbrSpecularGlossiness, explicit alphaMode, etc.)
    nlohmann::json gltf_json;
    bool has_gltf_json = false;
    std::ifstream gltf_in(filepath);
    if (gltf_in.is_open()) {
        try {
            gltf_in >> gltf_json;
            has_gltf_json = true;
        } catch (...) {}
    }

    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        aiMaterial* mat = scene->mMaterials[i];
        RawMaterial rm;
        aiString name;
        if (mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS)
            rm.name = name.C_Str();

        aiString tex_path;
        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &tex_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_BASE_COLOR, 0, &tex_path) == AI_SUCCESS) {
            std::string p = tex_path.C_Str();
            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0)
                p = base_dir + p;
            rm.base_color_texture_path = p;
            raw_mesh.textures.push_back(p);
        } else {
            rm.base_color_texture_path = raw_mesh.textures[0];
        }

        // 1. Detect Alpha Mode & Cutoff from glTF Material Properties
        rm.alpha_mode = bud::asset::AlphaMode::Opaque;
        rm.alpha_cutoff = 0.5f;
        rm.double_sided = false;

        aiString alpha_mode_str;
        if (mat->Get("$mat.gltf.alphaMode", 0, 0, alpha_mode_str) == AI_SUCCESS) {
            std::string mode_s = alpha_mode_str.C_Str();
            if (mode_s == "MASK") {
                rm.alpha_mode = bud::asset::AlphaMode::Mask;
            } else if (mode_s == "BLEND") {
                rm.alpha_mode = bud::asset::AlphaMode::Blend;
            }
        }

        int alpha_mode_int = 0;
        if (mat->Get("$mat.gltf.alphaMode", 0, 0, alpha_mode_int) == AI_SUCCESS) {
            if (alpha_mode_int == 1) {
                rm.alpha_mode = bud::asset::AlphaMode::Mask;
            } else if (alpha_mode_int == 2) {
                rm.alpha_mode = bud::asset::AlphaMode::Blend;
            }
        }

        float cutoff = 0.5f;
        if (mat->Get("$mat.gltf.alphaCutoff", 0, 0, cutoff) == AI_SUCCESS) {
            rm.alpha_cutoff = cutoff;
        }

        float opacity = 1.0f;
        if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
            if (opacity < 0.99f && rm.alpha_mode == bud::asset::AlphaMode::Opaque) {
                rm.alpha_mode = bud::asset::AlphaMode::Blend;
            }
        }

        int two_sided = 0;
        if (mat->Get(AI_MATKEY_TWOSIDED, two_sided) == AI_SUCCESS) {
            rm.double_sided = (two_sided != 0);
        }

        // 1. PBR Textures from Assimp
        aiString norm_path;
        if (mat->GetTexture(aiTextureType_NORMALS, 0, &norm_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_HEIGHT, 0, &norm_path) == AI_SUCCESS) {
            std::string p = norm_path.C_Str();
            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) p = base_dir + p;
            rm.normal_texture_path = p;
        }

        aiString rough_path;
        if (mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &rough_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_SHININESS, 0, &rough_path) == AI_SUCCESS ||
            mat->GetTexture(aiTextureType_METALNESS, 0, &rough_path) == AI_SUCCESS) {
            std::string p = rough_path.C_Str();
            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) p = base_dir + p;
            rm.metallic_roughness_texture_path = p;
        }

        aiString emissive_path;
        if (mat->GetTexture(aiTextureType_EMISSIVE, 0, &emissive_path) == AI_SUCCESS) {
            std::string p = emissive_path.C_Str();
            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) p = base_dir + p;
            rm.emissive_texture_path = p;
        }

        float metallic_factor = 0.0f;
        if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic_factor) == AI_SUCCESS) {
            rm.metallic_factor = metallic_factor;
        }
        float roughness_factor = 0.5f;
        if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness_factor) == AI_SUCCESS) {
            rm.roughness_factor = roughness_factor;
        }

        // 2. glTF JSON Extension & Fallback Extraction
        if (has_gltf_json && gltf_json.contains("materials") && i < gltf_json["materials"].size()) {
            const auto& gj_mat = gltf_json["materials"][i];

            // Resolve diffuse/albedo texture from extensions
            if (rm.base_color_texture_path.empty() || rm.base_color_texture_path == raw_mesh.textures[0]) {
                int tex_idx = -1;
                if (gj_mat.contains("pbrMetallicRoughness") && gj_mat["pbrMetallicRoughness"].contains("baseColorTexture")) {
                    tex_idx = gj_mat["pbrMetallicRoughness"]["baseColorTexture"].value("index", -1);
                } else if (gj_mat.contains("extensions") && gj_mat["extensions"].contains("KHR_materials_pbrSpecularGlossiness")) {
                    const auto& spec_gloss = gj_mat["extensions"]["KHR_materials_pbrSpecularGlossiness"];
                    if (spec_gloss.contains("diffuseTexture")) {
                        tex_idx = spec_gloss["diffuseTexture"].value("index", -1);
                    }
                }

                if (tex_idx >= 0 && gltf_json.contains("textures") && tex_idx < static_cast<int>(gltf_json["textures"].size())) {
                    int img_idx = gltf_json["textures"][tex_idx].value("source", -1);
                    if (img_idx >= 0 && gltf_json.contains("images") && img_idx < static_cast<int>(gltf_json["images"].size())) {
                        std::string uri = gltf_json["images"][img_idx].value("uri", "");
                        if (!uri.empty()) {
                            std::string p = uri;
                            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0)
                                p = base_dir + p;
                            rm.base_color_texture_path = p;
                            raw_mesh.textures.push_back(p);
                        }
                    }
                }
            }

            if (rm.normal_texture_path.empty() && gj_mat.contains("normalTexture")) {
                int tex_idx = gj_mat["normalTexture"].value("index", -1);
                if (tex_idx >= 0 && gltf_json.contains("textures") && tex_idx < static_cast<int>(gltf_json["textures"].size())) {
                    int img_idx = gltf_json["textures"][tex_idx].value("source", -1);
                    if (img_idx >= 0 && gltf_json.contains("images") && img_idx < static_cast<int>(gltf_json["images"].size())) {
                        std::string uri = gltf_json["images"][img_idx].value("uri", "");
                        if (!uri.empty()) {
                            std::string p = uri;
                            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) p = base_dir + p;
                            rm.normal_texture_path = p;
                        }
                    }
                }
            }

            if (rm.metallic_roughness_texture_path.empty() && gj_mat.contains("pbrMetallicRoughness") && gj_mat["pbrMetallicRoughness"].contains("metallicRoughnessTexture")) {
                int tex_idx = gj_mat["pbrMetallicRoughness"]["metallicRoughnessTexture"].value("index", -1);
                if (tex_idx >= 0 && gltf_json.contains("textures") && tex_idx < static_cast<int>(gltf_json["textures"].size())) {
                    int img_idx = gltf_json["textures"][tex_idx].value("source", -1);
                    if (img_idx >= 0 && gltf_json.contains("images") && img_idx < static_cast<int>(gltf_json["images"].size())) {
                        std::string uri = gltf_json["images"][img_idx].value("uri", "");
                        if (!uri.empty()) {
                            std::string p = uri;
                            if (p.find(":") == std::string::npos && p.find("/") != 0 && p.find("\\") != 0) p = base_dir + p;
                            rm.metallic_roughness_texture_path = p;
                        }
                    }
                }
            }

            if (gj_mat.contains("pbrMetallicRoughness")) {
                if (gj_mat["pbrMetallicRoughness"].contains("metallicFactor"))
                    rm.metallic_factor = gj_mat["pbrMetallicRoughness"]["metallicFactor"].get<float>();
                if (gj_mat["pbrMetallicRoughness"].contains("roughnessFactor"))
                    rm.roughness_factor = gj_mat["pbrMetallicRoughness"]["roughnessFactor"].get<float>();
            }

            if (gj_mat.contains("alphaMode")) {
                std::string mode_str = gj_mat["alphaMode"].get<std::string>();
                if (mode_str == "MASK") {
                    rm.alpha_mode = bud::asset::AlphaMode::Mask;
                } else if (mode_str == "BLEND") {
                    rm.alpha_mode = bud::asset::AlphaMode::Blend;
                } else if (mode_str == "OPAQUE") {
                    rm.alpha_mode = bud::asset::AlphaMode::Opaque;
                }
            }

            if (gj_mat.contains("alphaCutoff")) {
                rm.alpha_cutoff = gj_mat["alphaCutoff"].get<float>();
            }

            if (gj_mat.contains("doubleSided")) {
                rm.double_sided = gj_mat["doubleSided"].get<bool>();
            }
        }

        // 3. Auto-scan companion PBR textures (_N, _R, _M, _E)
        if (!rm.base_color_texture_path.empty() && rm.base_color_texture_path != raw_mesh.textures[0]) {
            auto companions = TextureImporter::find_companion_pbr_textures(rm.base_color_texture_path);
            if (rm.normal_texture_path.empty()) rm.normal_texture_path = companions.normal_path;
            if (rm.metallic_roughness_texture_path.empty()) {
                if (!companions.roughness_path.empty()) rm.metallic_roughness_texture_path = companions.roughness_path;
                else if (!companions.metallic_path.empty()) rm.metallic_roughness_texture_path = companions.metallic_path;
            }
            if (rm.emissive_texture_path.empty()) rm.emissive_texture_path = companions.emissive_path;
        }

        // 4. Texture Pixel Alpha Analysis (100% Data-driven):
        if (rm.alpha_mode == bud::asset::AlphaMode::Opaque && !rm.base_color_texture_path.empty() && rm.base_color_texture_path != raw_mesh.textures[0]) {
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

    for (const auto& inst : instances) {
        const aiMesh* mesh = scene->mMeshes[inst.mesh_index];
        uint32_t vertex_base = static_cast<uint32_t>(raw_mesh.vertices.size());
        uint32_t index_base = static_cast<uint32_t>(raw_mesh.indices.size());

        aiMatrix3x3 normal_matrix(inst.transform);
        normal_matrix.Inverse().Transpose();

        for (unsigned int v = 0; v < mesh->mNumVertices; ++v) {
            RawVertex rv{};
            aiVector3D pos = inst.transform * mesh->mVertices[v];
            rv.position[0] = pos.x;
            rv.position[1] = pos.y;
            rv.position[2] = pos.z;

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
