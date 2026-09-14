#include "bud.raw_mesh.hpp"
#include <algorithm>
#include <cfloat>
#include <fstream>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace bud::asset {

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

        auto write_str = [&](const std::string& str) {
            uint32_t len = static_cast<uint32_t>(str.length());
            write_bytes(&len, sizeof(len));
            if (len > 0)
                write_bytes(str.data(), len);
        };

        write_str(mat.base_color_texture_path);
        write_str(mat.normal_texture_path);
        write_str(mat.metallic_roughness_texture_path);
        write_str(mat.emissive_texture_path);

        write_bytes(&mat.metallic_factor, sizeof(mat.metallic_factor));
        write_bytes(&mat.roughness_factor, sizeof(mat.roughness_factor));
        write_bytes(mat.base_color_factor, sizeof(float) * 4);
        write_bytes(mat.emissive_factor, sizeof(float) * 3);

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
        if (offset + sz > size)
            return false;
        std::memcpy(dst, data + offset, sz);
        offset += sz;
        return true;
    };

    uint64_t magic = 0;
    uint32_t version = 0;
    if (!read_bytes(&magic, sizeof(magic)) || !read_bytes(&version, sizeof(version)))
        return std::nullopt;

    if (magic != BUD_RAWMESH_MAGIC || (version != 1 && version != BUD_RAWMESH_VERSION))
        return std::nullopt;

    RawMesh mesh;
    uint32_t path_len = 0;
    if (!read_bytes(&path_len, sizeof(path_len)))
        return std::nullopt;
    if (path_len > 0) {
        if (offset + path_len > size)
            return std::nullopt;
        mesh.source_path.assign(reinterpret_cast<const char*>(data + offset), path_len);
        offset += path_len;
    }

    if (!read_bytes(mesh.aabb_min, sizeof(float) * 3) || !read_bytes(mesh.aabb_max, sizeof(float) * 3))
        return std::nullopt;

    uint64_t vert_count = 0;
    if (!read_bytes(&vert_count, sizeof(vert_count)))
        return std::nullopt;
    if (vert_count > 0) {
        size_t vert_bytes = vert_count * sizeof(RawVertex);
        if (offset + vert_bytes > size)
            return std::nullopt;
        mesh.vertices.resize(vert_count);
        std::memcpy(mesh.vertices.data(), data + offset, vert_bytes);
        offset += vert_bytes;
    }

    uint64_t idx_count = 0;
    if (!read_bytes(&idx_count, sizeof(idx_count)))
        return std::nullopt;
    if (idx_count > 0) {
        size_t idx_bytes = idx_count * sizeof(uint32_t);
        if (offset + idx_bytes > size)
            return std::nullopt;
        mesh.indices.resize(idx_count);
        std::memcpy(mesh.indices.data(), data + offset, idx_bytes);
        offset += idx_bytes;
    }

    uint32_t submesh_count = 0;
    if (!read_bytes(&submesh_count, sizeof(submesh_count)))
        return std::nullopt;
    mesh.submeshes.resize(submesh_count);
    for (uint32_t i = 0; i < submesh_count; ++i) {
        uint32_t name_len = 0;
        if (!read_bytes(&name_len, sizeof(name_len)))
            return std::nullopt;
        if (name_len > 0) {
            if (offset + name_len > size)
                return std::nullopt;
            mesh.submeshes[i].name.assign(reinterpret_cast<const char*>(data + offset), name_len);
            offset += name_len;
        }
        if (!read_bytes(&mesh.submeshes[i].index_offset, sizeof(uint32_t)) ||
            !read_bytes(&mesh.submeshes[i].index_count, sizeof(uint32_t)) ||
            !read_bytes(&mesh.submeshes[i].material_index, sizeof(uint32_t)))
            return std::nullopt;
    }

    uint32_t mat_count = 0;
    if (!read_bytes(&mat_count, sizeof(mat_count)))
        return std::nullopt;
    mesh.materials.resize(mat_count);
    for (uint32_t i = 0; i < mat_count; ++i) {
        uint32_t name_len = 0;
        if (!read_bytes(&name_len, sizeof(name_len)))
            return std::nullopt;
        if (name_len > 0) {
            if (offset + name_len > size)
                return std::nullopt;
            mesh.materials[i].name.assign(reinterpret_cast<const char*>(data + offset), name_len);
            offset += name_len;
        }

        auto read_str = [&](std::string& out_str) -> bool {
            uint32_t len = 0;
            if (!read_bytes(&len, sizeof(len)))
                return false;
            if (len > 0) {
                if (offset + len > size)
                    return false;
                out_str.assign(reinterpret_cast<const char*>(data + offset), len);
                offset += len;
            } else {
                out_str.clear();
            }
            return true;
        };

        if (!read_str(mesh.materials[i].base_color_texture_path))
            return std::nullopt;

        if (version >= 2) {
            if (!read_str(mesh.materials[i].normal_texture_path))
                return std::nullopt;
            if (!read_str(mesh.materials[i].metallic_roughness_texture_path))
                return std::nullopt;
            if (!read_str(mesh.materials[i].emissive_texture_path))
                return std::nullopt;

            if (!read_bytes(&mesh.materials[i].metallic_factor, sizeof(float)))
                return std::nullopt;
            if (!read_bytes(&mesh.materials[i].roughness_factor, sizeof(float)))
                return std::nullopt;
            if (!read_bytes(mesh.materials[i].base_color_factor, sizeof(float) * 4))
                return std::nullopt;
            if (!read_bytes(mesh.materials[i].emissive_factor, sizeof(float) * 3))
                return std::nullopt;
        }

        uint8_t alpha_mode = 0;
        if (!read_bytes(&alpha_mode, sizeof(alpha_mode)))
            return std::nullopt;
        mesh.materials[i].alpha_mode = static_cast<bud::asset::AlphaMode>(alpha_mode);

        uint8_t double_sided = 0;
        if (!read_bytes(&double_sided, sizeof(double_sided)))
            return std::nullopt;
        mesh.materials[i].double_sided = (double_sided != 0);

        if (!read_bytes(&mesh.materials[i].alpha_cutoff, sizeof(float)))
            return std::nullopt;
    }

    uint32_t tex_count = 0;
    if (!read_bytes(&tex_count, sizeof(tex_count)))
        return std::nullopt;
    mesh.textures.resize(tex_count);
    for (uint32_t i = 0; i < tex_count; ++i) {
        uint32_t tex_len = 0;
        if (!read_bytes(&tex_len, sizeof(tex_len)))
            return std::nullopt;
        if (tex_len > 0) {
            if (offset + tex_len > size)
                return std::nullopt;
            mesh.textures[i].assign(reinterpret_cast<const char*>(data + offset), tex_len);
            offset += tex_len;
        }
    }

    return mesh;
}

bool RawMesh::save_binary(const std::string& path) const {
    auto data = serialize_binary();
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file.good();
}

std::optional<RawMesh> RawMesh::load_binary(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return std::nullopt;

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
        return std::nullopt;

    return deserialize_binary(buffer.data(), buffer.size());
}

bool RawMesh::save_json(const std::string& path) const {
    nlohmann::json j;
    j["source_path"] = source_path;
    j["aabb_min"] = { aabb_min[0], aabb_min[1], aabb_min[2] };
    j["aabb_max"] = { aabb_max[0], aabb_max[1], aabb_max[2] };

    auto& j_verts = j["vertices"];
    for (const auto& v : vertices) {
        j_verts.push_back({
            { "position", { v.position[0], v.position[1], v.position[2] } },
            { "normal", { v.normal[0], v.normal[1], v.normal[2] } },
            { "uv", { v.uv[0], v.uv[1] } },
            { "tangent", { v.tangent[0], v.tangent[1], v.tangent[2], v.tangent[3] } }
        });
    }

    j["indices"] = indices;

    auto& j_submeshes = j["submeshes"];
    for (const auto& sm : submeshes) {
        j_submeshes.push_back({
            { "name", sm.name },
            { "index_offset", sm.index_offset },
            { "index_count", sm.index_count },
            { "material_index", sm.material_index }
        });
    }

    auto& j_materials = j["materials"];
    for (const auto& mat : materials) {
        j_materials.push_back({
            { "name", mat.name },
            { "base_color_texture_path", mat.base_color_texture_path },
            { "alpha_mode", static_cast<uint8_t>(mat.alpha_mode) },
            { "double_sided", mat.double_sided },
            { "alpha_cutoff", mat.alpha_cutoff }
        });
    }

    j["textures"] = textures;

    std::ofstream file(path);
    if (!file.is_open())
        return false;

    file << j.dump(2);
    return file.good();
}

std::optional<RawMesh> RawMesh::load_json(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open())
        return std::nullopt;

    nlohmann::json j;
    try {
        file >> j;
    } catch (...) {
        return std::nullopt;
    }

    RawMesh mesh;
    if (j.contains("source_path"))
        mesh.source_path = j["source_path"].get<std::string>();

    if (j.contains("aabb_min")) {
        for (int i = 0; i < 3; ++i)
            mesh.aabb_min[i] = j["aabb_min"][i].get<float>();
    }
    if (j.contains("aabb_max")) {
        for (int i = 0; i < 3; ++i)
            mesh.aabb_max[i] = j["aabb_max"][i].get<float>();
    }

    if (j.contains("vertices")) {
        for (const auto& j_v : j["vertices"]) {
            RawVertex v;
            for (int i = 0; i < 3; ++i)
                v.position[i] = j_v["position"][i].get<float>();
            for (int i = 0; i < 3; ++i)
                v.normal[i] = j_v["normal"][i].get<float>();
            for (int i = 0; i < 2; ++i)
                v.uv[i] = j_v["uv"][i].get<float>();
            for (int i = 0; i < 4; ++i)
                v.tangent[i] = j_v["tangent"][i].get<float>();
            mesh.vertices.push_back(v);
        }
    }

    if (j.contains("indices"))
        mesh.indices = j["indices"].get<std::vector<uint32_t>>();

    if (j.contains("submeshes")) {
        for (const auto& j_sm : j["submeshes"]) {
            RawSubmesh sm;
            sm.name = j_sm["name"].get<std::string>();
            sm.index_offset = j_sm["index_offset"].get<uint32_t>();
            sm.index_count = j_sm["index_count"].get<uint32_t>();
            sm.material_index = j_sm["material_index"].get<uint32_t>();
            mesh.submeshes.push_back(sm);
        }
    }

    if (j.contains("materials")) {
        for (const auto& j_mat : j["materials"]) {
            RawMaterial mat;
            mat.name = j_mat["name"].get<std::string>();
            mat.base_color_texture_path = j_mat["base_color_texture_path"].get<std::string>();
            mat.alpha_mode = static_cast<bud::asset::AlphaMode>(j_mat["alpha_mode"].get<uint8_t>());
            mat.double_sided = j_mat["double_sided"].get<bool>();
            mat.alpha_cutoff = j_mat["alpha_cutoff"].get<float>();
            mesh.materials.push_back(mat);
        }
    }

    if (j.contains("textures"))
        mesh.textures = j["textures"].get<std::vector<std::string>>();

    return mesh;
}

std::optional<RawMesh> RawMesh::load_from_file(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".rawmesh" || ext == ".bin")
        return load_binary(path);
    else if (ext == ".json")
        return load_json(path);
    return std::nullopt;
}

} // namespace bud::asset
