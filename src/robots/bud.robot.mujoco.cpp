#include "src/robots/bud.robot.mujoco.hpp"

#include <cstring>

namespace bud::robots {

    namespace {

        void write_u32(std::vector<uint8_t>& out, uint32_t value) {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        }

        void write_string(std::vector<uint8_t>& out, const std::string& text) {
            write_u32(out, static_cast<uint32_t>(text.size()));
            out.insert(out.end(), text.begin(), text.end());
        }

        bool read_u32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) {
            if (offset + 4 > size)
                return false;
            std::memcpy(&value, data + offset, 4);
            offset += 4;
            return true;
        }

        bool read_string(const uint8_t* data, size_t size, size_t& offset, std::string& text) {
            uint32_t length = 0;
            if (!read_u32(data, size, offset, length))
                return false;
            if (offset + length > size)
                return false;
            text.assign(reinterpret_cast<const char*>(data + offset), length);
            offset += length;
            return true;
        }

    } // namespace

    std::vector<uint8_t> MujocoModelData::serialize_binary() const {
        std::vector<uint8_t> out;
        write_u32(out, version);
        write_u32(out, static_cast<uint32_t>(format));
        write_string(out, model_payload);
        write_string(out, render_metadata_json);
        write_string(out, physics_metadata_json);
        write_u32(out, static_cast<uint32_t>(meshes.size()));
        for (const auto& mesh : meshes) {
            write_string(out, mesh.mjcf_name);
            write_string(out, mesh.asset_path);
        }
        return out;
    }

    std::optional<MujocoModelData> MujocoModelData::deserialize_binary(const uint8_t* data, size_t size) {
        if (!data || size < 4)
            return std::nullopt;

        MujocoModelData result;
        size_t offset = 0;
        uint32_t format_value = 0;
        if (!read_u32(data, size, offset, result.version) ||
            !read_u32(data, size, offset, format_value) ||
            !read_string(data, size, offset, result.model_payload) ||
            !read_string(data, size, offset, result.render_metadata_json) ||
            !read_string(data, size, offset, result.physics_metadata_json))
            return std::nullopt;
        result.format = (format_value == static_cast<uint32_t>(MujocoModelFormat::Mjb))
                            ? MujocoModelFormat::Mjb
                            : MujocoModelFormat::Mjcf;

        uint32_t mesh_count = 0;
        if (!read_u32(data, size, offset, mesh_count))
            return std::nullopt;

        result.meshes.reserve(mesh_count);
        for (uint32_t i = 0; i < mesh_count; ++i) {
            MujocoMeshRef mesh;
            if (!read_string(data, size, offset, mesh.mjcf_name) ||
                !read_string(data, size, offset, mesh.asset_path))
                return std::nullopt;
            result.meshes.push_back(std::move(mesh));
        }
        return result;
    }

} // namespace bud::robots
