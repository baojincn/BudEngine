#pragma once

#include <nlohmann/json.hpp>
#include <sstream>
#include "bud.scene.hpp"

namespace glm {
    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const vec2& v) {
        j = BasicJsonType{ v.x, v.y };
    }
    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, vec2& v) {
        v.x = j.at(0).template get<float>();
        v.y = j.at(1).template get<float>();
    }

    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const vec3& v) {
        j = BasicJsonType{ v.x, v.y, v.z };
    }
    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, vec3& v) {
        v.x = j.at(0).template get<float>();
        v.y = j.at(1).template get<float>();
        v.z = j.at(2).template get<float>();
    }

    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const mat4& m) {
        j = BasicJsonType::array();
        for (int i = 0; i < 4; ++i)
            for (int j_ = 0; j_ < 4; ++j_)
                j.push_back(m[i][j_]);
    }
    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, mat4& m) {
        for (int i = 0; i < 4; ++i)
            for (int j_ = 0; j_ < 4; ++j_)
                m[i][j_] = j.at(i * 4 + j_).template get<float>();
    }
}

namespace bud::scene {

    // Custom JSON dump: objects indented, arrays compact (single line)
    inline void dump_json(std::ostream& os, const nlohmann::ordered_json& j, int indent = 0, int step = 4) {
        if (j.is_object()) {
            os << "{";
            if (!j.empty()) {
                os << "\n";
                bool first = true;
                for (auto& [key, value] : j.items()) {
                    if (!first) os << ",\n";
                    first = false;
                    os << std::string(indent + step, ' ') << "\"" << key << "\": ";
                    dump_json(os, value, indent + step, step);
                }
                os << "\n" << std::string(indent, ' ');
            }
            os << "}";
        } else if (j.is_array()) {
            if (j.empty()) {
                os << "[]";
                return;
            }
            bool all_primitive = true;
            for (auto& val : j) {
                if (!val.is_number() && !val.is_boolean() && !val.is_null()) {
                    all_primitive = false;
                    break;
                }
            }
            if (all_primitive) {
                os << "[";
                bool first = true;
                for (auto& val : j) {
                    if (!first) os << ", ";
                    first = false;
                    dump_json(os, val, indent, step);
                }
                os << "]";
            } else {
                os << "[\n";
                bool first = true;
                for (auto& val : j) {
                    if (!first) os << ",\n";
                    first = false;
                    os << std::string(indent + step, ' ');
                    dump_json(os, val, indent + step, step);
                }
                os << "\n" << std::string(indent, ' ') << "]";
            }
        } else if (j.is_string()) {
            os << "\"" << j.get<std::string>() << "\"";
        } else if (j.is_boolean()) {
            os << (j.get<bool>() ? "true" : "false");
        } else if (j.is_number_float()) {
            os << j.get<double>();
        } else if (j.is_number_integer()) {
            os << j.get<int64_t>();
        } else if (j.is_number_unsigned()) {
            os << j.get<uint64_t>();
        } else if (j.is_null()) {
            os << "null";
        }
    }

    inline std::string dump_scene(const nlohmann::ordered_json& j) {
        std::ostringstream oss;
        dump_json(oss, j);
        return oss.str();
    }

    // Entity
    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const Entity& e) {
        j = BasicJsonType{
            {"asset_path", e.asset_path},
            {"is_active", e.is_active},
            {"is_static", e.is_static},
            {"is_cast_shadow", e.is_cast_shadow},
            {"is_receive_shadow", e.is_receive_shadow},
            {"enable_physics", e.enable_physics},
            {"material_index", e.material_index},
            {"mesh_index", e.mesh_index},
            {"name", e.name},
            {"transform", e.transform}
        };
    }
    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, Entity& e) {
        if (j.contains("name")) j.at("name").get_to(e.name);
        if (j.contains("asset_path")) j.at("asset_path").get_to(e.asset_path);
        if (j.contains("mesh_index")) j.at("mesh_index").get_to(e.mesh_index);
        if (j.contains("material_index")) j.at("material_index").get_to(e.material_index);
        if (j.contains("transform")) j.at("transform").get_to(e.transform);
        if (j.contains("is_static")) j.at("is_static").get_to(e.is_static);
        if (j.contains("is_active")) j.at("is_active").get_to(e.is_active);
        if (j.contains("is_cast_shadow")) j.at("is_cast_shadow").get_to(e.is_cast_shadow);
        if (j.contains("is_receive_shadow")) j.at("is_receive_shadow").get_to(e.is_receive_shadow);
        if (j.contains("enable_physics")) j.at("enable_physics").get_to(e.enable_physics);
        if (j.contains("lod_bias")) j.at("lod_bias").get_to(e.lod_bias);
    }

    // Camera
    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const Camera& c) {
        j = BasicJsonType{
            {"pitch", c.pitch},
            {"position", c.position},
            {"sensitivity", c.mouse_sensitivity},
            {"speed", c.movement_speed},
            {"yaw", c.yaw},
            {"zoom", c.zoom}
        };
    }
    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, Camera& c) {
        if (j.contains("position")) j.at("position").get_to(c.position);
        if (j.contains("yaw")) j.at("yaw").get_to(c.yaw);
        if (j.contains("pitch")) j.at("pitch").get_to(c.pitch);
        if (j.contains("zoom")) j.at("zoom").get_to(c.zoom);
        if (j.contains("speed")) j.at("speed").get_to(c.movement_speed);
        if (j.contains("sensitivity")) j.at("sensitivity").get_to(c.mouse_sensitivity);

        c.rebuild_camera_vectors();
    }

    // DirectionalLight
    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const DirectionalLight& l) {
        j = BasicJsonType{
            {"color", l.color},
            {"direction", l.direction},
            {"intensity", l.intensity}
        };
    }
    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, DirectionalLight& l) {
        if (j.contains("direction")) j.at("direction").get_to(l.direction);
        if (j.contains("color")) j.at("color").get_to(l.color);
        if (j.contains("intensity")) j.at("intensity").get_to(l.intensity);
    }

    // Scene
    template <typename BasicJsonType>
    inline void to_json(BasicJsonType& j, const Scene& s) {
        j = BasicJsonType{
            {"ambient_strength", s.ambient_strength},
            {"directional_light", s.directional_light},
            {"lod_error_threshold_px", s.lod_error_threshold_px},
            {"main_camera", s.main_camera},
            {"streaming_unload_radius", s.streaming_unload_radius},
            {"entities", s.entities}
        };
    }

    template <typename BasicJsonType>
    inline void from_json(const BasicJsonType& j, Scene& s) {
        if (j.contains("ambient_strength")) j.at("ambient_strength").get_to(s.ambient_strength);
        if (j.contains("directional_light")) j.at("directional_light").get_to(s.directional_light);
        if (j.contains("lod_error_threshold_px")) j.at("lod_error_threshold_px").get_to(s.lod_error_threshold_px);
        if (j.contains("main_camera")) j.at("main_camera").get_to(s.main_camera);
        if (j.contains("streaming_unload_radius")) j.at("streaming_unload_radius").get_to(s.streaming_unload_radius);
        if (j.contains("entities")) j.at("entities").get_to(s.entities);
    }
}
