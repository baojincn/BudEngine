#include "raw_mesh.hpp"
#include <algorithm>
#include <cfloat>
#include <fstream>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace bud::asset_pipeline {

void RawMesh::compute_bounds() {
    if (vertices.empty()) {
        aabb_min[0] = aabb_min[1] = aabb_min[2] = 0.0f;
        aabb_max[0] = aabb_max[1] = aabb_max[2] = 0.0f;
        return;
    }

    aabb_min[0] = aabb_min[1] = aabb_min[2] = FLT_MAX;
    aabb_max[0] = aabb_max[1] = aabb_max[2] = -FLT_MAX;

    for (const auto& v : vertices) {
        for (int i = 0; i < 3; ++i) {
            aabb_min[i] = std::min(aabb_min[i], v.position[i]);
            aabb_max[i] = std::max(aabb_max[i], v.position[i]);
        }
    }
}

std::vector<uint8_t> RawMesh::serialize_binary() const {
    std::vector<uint8_t> buffer;

    auto write_bytes = [&](const void* ptr, size_t sz) {
        const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(ptr);
        buffer.insert(buffer.end(), byte_ptr, byte_ptr + sz);
    };

    uint64_t magic = BUD_RAWMESH_MAGIC;
    uint32_t version = BUD_RAWMESH_VERSION;
    write_bytes(&magic, sizeof(magic));
    write_bytes(&version, sizeof(version));

    uint32_t path_len = static_cast<uint32_t>(source_path.length());
    write_bytes(&path_len, sizeof(path_len));
    if (path_len > 0)
        write_bytes(source_path.data(), path_len);

    write_bytes(aabb_min, sizeof(float) * 3);
    write_bytes(aabb_max, sizeof(float) * 3);

    // Vertices
    uint64_t vert_count = vertices.size();
    write_bytes(&vert_count, sizeof(vert_count));
    if (vert_count > 0)
        write_bytes(vertices.data(), vert_count * sizeof(RawVertex));

    // Indices
    uint64_t idx_count = indices.size();
    write_bytes(&idx_count, sizeof(idx_count));
    if (idx_count > 0)
        write_bytes(indices.data(), idx_count * sizeof(uint32_t));

    // Submeshes
    uint32_t submesh_count = static_cast<uint32_t>(submeshes.size());
    write_bytes(&submesh_count, sizeof(submesh_count));
    for (const auto& sm : submeshes) {
        uint32_t name_len = static_cast<uint32_t>(sm.name.length());
        write_bytes(&name_len, sizeof(name_len));
        if (name_len > 0)
            write_bytes(sm.name.data(), name_len);
        write_bytes(&sm.index_offset, sizeof(sm.index_offset));
        write_bytes(&sm.index_count, sizeof(sm.index_count));
        write_bytes(&sm.material_index, sizeof(sm.material_index));
    }

    // Materials
    uint32_t mat_count = static_cast<uint32_t>(materials.size());
    write_bytes(&mat_count, sizeof(mat_count));
    for (const auto& mat : materials) {
        uint32_t name_len = static_cast<uint32_t>(mat.name.length());
        write_bytes(&name_len, sizeof(name_len));
        if (name_len > 0)
            write_bytes(mat.name.data(), name_len);

        uint32_t tex_len = static_cast<uint32_t>(mat.base_color_texture_path.length());
        write_bytes(&tex_len, sizeof(tex_len));
        if (tex_len > 0)
            write_bytes(mat.base_color_texture_path.data(), tex_len);

        uint8_t alpha_mode = static_cast<uint8_t>(mat.alpha_mode);
        write_bytes(&alpha_mode, sizeof(alpha_mode));
        uint8_t double_sided = mat.double_sided ? 1 : 0;
        write_bytes(&double_sided, sizeof(double_sided));
        write_bytes(&mat.alpha_cutoff, sizeof(mat.alpha_cutoff));
    }

    // Textures
    uint32_t tex_count = static_cast<uint32_t>(textures.size());
    write_bytes(&tex_count, sizeof(tex_count));
    for (const auto& tex : textures) {
        uint32_t tlen = static_cast<uint32_t>(tex.length());
        write_bytes(&tlen, sizeof(tlen));
        if (tlen > 0)
            write_bytes(tex.data(), tlen);
    }

    return buffer;
}

std::optional<RawMesh> RawMesh::deserialize_binary(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(uint64_t) + sizeof(uint32_t))
        return std::nullopt;

    size_t offset = 0;
    auto read_bytes = [&](void* dst, size_t sz) -> bool {
        if (offset + sz > size) return false;
        std::memcpy(dst, data + offset, sz);
        offset += sz;
        return true;
    };

    uint64_t magic = 0;
    uint32_t version = 0;
    if (!read_bytes(&magic, sizeof(magic)) || !read_bytes(&version, sizeof(version)))
        return std::nullopt;

    if (magic != BUD_RAWMESH_MAGIC || version != BUD_RAWMESH_VERSION)
        return std::nullopt;

    RawMesh mesh;
    uint32_t path_len = 0;
    if (!read_bytes(&path_len, sizeof(path_len))) return std::nullopt;
    if (path_len > 0) {
        if (offset + path_len > size) return std::nullopt;
        mesh.source_path.assign(reinterpret_cast<const char*>(data + offset), path_len);
        offset += path_len;
    }

    if (!read_bytes(mesh.aabb_min, sizeof(float) * 3) || !read_bytes(mesh.aabb_max, sizeof(float) * 3))
        return std::nullopt;

    uint64_t vert_count = 0;
    if (!read_bytes(&vert_count, sizeof(vert_count))) return std::nullopt;
    if (vert_count > 0) {
        size_t vert_bytes = vert_count * sizeof(RawVertex);
        if (offset + vert_bytes > size) return std::nullopt;
        mesh.vertices.resize(vert_count);
        std::memcpy(mesh.vertices.data(), data + offset, vert_bytes);
        offset += vert_bytes;
    }

    uint64_t idx_count = 0;
    if (!read_bytes(&idx_count, sizeof(idx_count))) return std::nullopt;
    if (idx_count > 0) {
        size_t idx_bytes = idx_count * sizeof(uint32_t);
        if (offset + idx_bytes > size) return std::nullopt;
        mesh.indices.resize(idx_count);
        std::memcpy(mesh.indices.data(), data + offset, idx_bytes);
        offset += idx_bytes;
    }

    uint32_t submesh_count = 0;
    if (!read_bytes(&submesh_count, sizeof(submesh_count))) return std::nullopt;
    mesh.submeshes.resize(submesh_count);
    for (uint32_t i = 0; i < submesh_count; ++i) {
        uint32_t name_len = 0;
        if (!read_bytes(&name_len, sizeof(name_len))) return std::nullopt;
        if (name_len > 0) {
            if (offset + name_len > size) return std::nullopt;
            mesh.submeshes[i].name.assign(reinterpret_cast<const char*>(data + offset), name_len);
            offset += name_len;
        }
        if (!read_bytes(&mesh.submeshes[i].index_offset, sizeof(uint32_t)) ||
            !read_bytes(&mesh.submeshes[i].index_count, sizeof(uint32_t)) ||
            !read_bytes(&mesh.submeshes[i].material_index, sizeof(uint32_t)))
            return std::nullopt;
    }

    uint32_t mat_count = 0;
    if (!read_bytes(&mat_count, sizeof(mat_count))) return std::nullopt;
    mesh.materials.resize(mat_count);
    for (uint32_t i = 0; i < mat_count; ++i) {
        uint32_t name_len = 0;
        if (!read_bytes(&name_len, sizeof(name_len))) return std::nullopt;
        if (name_len > 0) {
            if (offset + name_len > size) return std::nullopt;
            mesh.materials[i].name.assign(reinterpret_cast<const char*>(data + offset), name_len);
            offset += name_len;
        }

        uint32_t tex_len = 0;
        if (!read_bytes(&tex_len, sizeof(tex_len))) return std::nullopt;
        if (tex_len > 0) {
            if (offset + tex_len > size) return std::nullopt;
            mesh.materials[i].base_color_texture_path.assign(reinterpret_cast<const char*>(data + offset), tex_len);
            offset += tex_len;
        }

        uint8_t alpha_mode = 0;
        if (!read_bytes(&alpha_mode, sizeof(alpha_mode))) return std::nullopt;
        mesh.materials[i].alpha_mode = static_cast<bud::asset::AlphaMode>(alpha_mode);

        uint8_t double_sided = 0;
        if (!read_bytes(&double_sided, sizeof(double_sided))) return std::nullopt;
        mesh.materials[i].double_sided = (double_sided != 0);

        if (!read_bytes(&mesh.materials[i].alpha_cutoff, sizeof(float))) return std::nullopt;
    }

    uint32_t tex_count = 0;
    if (!read_bytes(&tex_count, sizeof(tex_count))) return std::nullopt;
    mesh.textures.resize(tex_count);
    for (uint32_t i = 0; i < tex_count; ++i) {
        uint32_t tlen = 0;
        if (!read_bytes(&tlen, sizeof(tlen))) return std::nullopt;
        if (tlen > 0) {
            if (offset + tlen > size) return std::nullopt;
            mesh.textures[i].assign(reinterpret_cast<const char*>(data + offset), tlen);
            offset += tlen;
        }
    }

    return mesh;
}

bool RawMesh::save_binary(const std::string& path) const {
    std::vector<uint8_t> buffer = serialize_binary();
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "[BudAssetPipeline] Failed to open .rawmesh for writing: " << path << std::endl;
        return false;
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    out.flush();
    return out.good();
}

std::optional<RawMesh> RawMesh::load_binary(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return std::nullopt;

    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(buffer.data()), size))
        return std::nullopt;

    return deserialize_binary(buffer.data(), buffer.size());
}

bool RawMesh::save_json(const std::string& path) const {
    nlohmann::json j;
    j["magic"] = "BRAWMES_JSON";
    j["version"] = BUD_RAWMESH_VERSION;
    j["source_path"] = source_path;
    j["aabb_min"] = { aabb_min[0], aabb_min[1], aabb_min[2] };
    j["aabb_max"] = { aabb_max[0], aabb_max[1], aabb_max[2] };

    j["submeshes"] = nlohmann::json::array();
    for (const auto& sm : submeshes) {
        j["submeshes"].push_back({
            { "name", sm.name },
            { "index_offset", sm.index_offset },
            { "index_count", sm.index_count },
            { "material_index", sm.material_index }
        });
    }

    j["materials"] = nlohmann::json::array();
    for (const auto& mat : materials) {
        j["materials"].push_back({
            { "name", mat.name },
            { "base_color_texture", mat.base_color_texture_path },
            { "alpha_mode", static_cast<int>(mat.alpha_mode) },
            { "double_sided", mat.double_sided },
            { "alpha_cutoff", mat.alpha_cutoff }
        });
    }

    j["textures"] = textures;

    j["vertices"] = nlohmann::json::array();
    for (const auto& v : vertices) {
        j["vertices"].push_back({
            { "pos", { v.position[0], v.position[1], v.position[2] } },
            { "norm", { v.normal[0], v.normal[1], v.normal[2] } },
            { "uv", { v.uv[0], v.uv[1] } },
            { "tan", { v.tangent[0], v.tangent[1], v.tangent[2], v.tangent[3] } }
        });
    }

    j["indices"] = indices;

    std::ofstream out(path);
    if (!out.is_open())
        return false;

    out << j.dump(2);
    return out.good();
}

std::optional<RawMesh> RawMesh::load_json(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open())
        return std::nullopt;

    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return std::nullopt;
    }

    RawMesh mesh;
    mesh.source_path = j.value("source_path", "");
    if (j.contains("aabb_min") && j["aabb_min"].is_array() && j["aabb_min"].size() == 3) {
        mesh.aabb_min[0] = j["aabb_min"][0].get<float>();
        mesh.aabb_min[1] = j["aabb_min"][1].get<float>();
        mesh.aabb_min[2] = j["aabb_min"][2].get<float>();
    }
    if (j.contains("aabb_max") && j["aabb_max"].is_array() && j["aabb_max"].size() == 3) {
        mesh.aabb_max[0] = j["aabb_max"][0].get<float>();
        mesh.aabb_max[1] = j["aabb_max"][1].get<float>();
        mesh.aabb_max[2] = j["aabb_max"][2].get<float>();
    }

    if (j.contains("submeshes") && j["submeshes"].is_array()) {
        for (const auto& item : j["submeshes"]) {
            RawSubmesh sm;
            sm.name = item.value("name", "");
            sm.index_offset = item.value("index_offset", 0u);
            sm.index_count = item.value("index_count", 0u);
            sm.material_index = item.value("material_index", 0u);
            mesh.submeshes.push_back(sm);
        }
    }

    if (j.contains("materials") && j["materials"].is_array()) {
        for (const auto& item : j["materials"]) {
            RawMaterial mat;
            mat.name = item.value("name", "");
            mat.base_color_texture_path = item.value("base_color_texture", "");
            mat.alpha_mode = static_cast<bud::asset::AlphaMode>(item.value("alpha_mode", 0));
            mat.double_sided = item.value("double_sided", false);
            mat.alpha_cutoff = item.value("alpha_cutoff", 0.5f);
            mesh.materials.push_back(mat);
        }
    }

    if (j.contains("textures") && j["textures"].is_array()) {
        for (const auto& item : j["textures"])
            mesh.textures.push_back(item.get<std::string>());
    }

    if (j.contains("vertices") && j["vertices"].is_array()) {
        mesh.vertices.reserve(j["vertices"].size());
        for (const auto& item : j["vertices"]) {
            RawVertex v;
            if (item.contains("pos") && item["pos"].is_array()) {
                v.position[0] = item["pos"][0].get<float>();
                v.position[1] = item["pos"][1].get<float>();
                v.position[2] = item["pos"][2].get<float>();
            }
            if (item.contains("norm") && item["norm"].is_array()) {
                v.normal[0] = item["norm"][0].get<float>();
                v.normal[1] = item["norm"][1].get<float>();
                v.normal[2] = item["norm"][2].get<float>();
            }
            if (item.contains("uv") && item["uv"].is_array()) {
                v.uv[0] = item["uv"][0].get<float>();
                v.uv[1] = item["uv"][1].get<float>();
            }
            if (item.contains("tan") && item["tan"].is_array()) {
                v.tangent[0] = item["tan"][0].get<float>();
                v.tangent[1] = item["tan"][1].get<float>();
                v.tangent[2] = item["tan"][2].get<float>();
                v.tangent[3] = item["tan"][3].get<float>();
            }
            mesh.vertices.push_back(v);
        }
    }

    if (j.contains("indices") && j["indices"].is_array()) {
        mesh.indices = j["indices"].get<std::vector<uint32_t>>();
    }

    return mesh;
}

std::optional<RawMesh> RawMesh::load_from_file(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".json") {
        return load_json(path);
    }

    auto res = load_binary(path);
    if (res)
        return res;

    return load_json(path);
}

} // namespace bud::asset_pipeline
