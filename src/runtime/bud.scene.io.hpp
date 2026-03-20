#pragma once

#include <nlohmann/json.hpp>
#include "bud.scene.hpp"

// GLM Serialization Helpers
namespace glm {
    inline void to_json(nlohmann::json& j, const vec2& v) {
        j = nlohmann::json{ v.x, v.y };
    }
    inline void from_json(const nlohmann::json& j, vec2& v) {
        v.x = j.at(0).get<float>();
        v.y = j.at(1).get<float>();
    }

    inline void to_json(nlohmann::json& j, const vec3& v) {
        j = nlohmann::json{ v.x, v.y, v.z };
    }
    inline void from_json(const nlohmann::json& j, vec3& v) {
        v.x = j.at(0).get<float>();
        v.y = j.at(1).get<float>();
        v.z = j.at(2).get<float>();
    }

    inline void to_json(nlohmann::json& j, const mat4& m) {
        j = nlohmann::json::array();
        for (int i = 0; i < 4; ++i)
            for (int j_ = 0; j_ < 4; ++j_)
                j.push_back(m[i][j_]);
    }
    inline void from_json(const nlohmann::json& j, mat4& m) {
        for (int i = 0; i < 4; ++i)
            for (int j_ = 0; j_ < 4; ++j_)
                m[i][j_] = j.at(i * 4 + j_).get<float>();
    }
}

namespace bud::scene {
    // NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE requires the type to be in the same namespace or accessible
    // Entity
    inline void to_json(nlohmann::json& j, const Entity& e) {
        j = nlohmann::json{
            {"asset_path", e.asset_path},
            {"mesh_index", e.mesh_index},
            {"material_index", e.material_index},
            {"transform", e.transform},
            {"is_static", e.is_static},
            {"is_active", e.is_active}
        };
    }
    inline void from_json(const nlohmann::json& j, Entity& e) {
        j.at("asset_path").get_to(e.asset_path);
        j.at("mesh_index").get_to(e.mesh_index);
        j.at("material_index").get_to(e.material_index);
        j.at("transform").get_to(e.transform);
        j.at("is_static").get_to(e.is_static);
        j.at("is_active").get_to(e.is_active);
    }

    // Camera
    inline void to_json(nlohmann::json& j, const Camera& c) {
        j = nlohmann::json{
            {"position", c.position},
            {"yaw", c.yaw},
            {"pitch", c.pitch},
            {"zoom", c.zoom},
            {"speed", c.movement_speed},
            {"sensitivity", c.mouse_sensitivity}
        };
    }
    inline void from_json(const nlohmann::json& j, Camera& c) {
        j.at("position").get_to(c.position);
        j.at("yaw").get_to(c.yaw);
        j.at("pitch").get_to(c.pitch);
        j.at("zoom").get_to(c.zoom);
        if (j.contains("speed")) j.at("speed").get_to(c.movement_speed);
        if (j.contains("sensitivity")) j.at("sensitivity").get_to(c.mouse_sensitivity);
    }

    // DirectionalLight
    inline void to_json(nlohmann::json& j, const DirectionalLight& l) {
        j = nlohmann::json{
            {"direction", l.direction},
            {"color", l.color},
            {"intensity", l.intensity}
        };
    }
    inline void from_json(const nlohmann::json& j, DirectionalLight& l) {
        j.at("direction").get_to(l.direction);
        j.at("color").get_to(l.color);
        j.at("intensity").get_to(l.intensity);
    }

    // Scene
    inline void to_json(nlohmann::json& j, const Scene& s) {
        j = nlohmann::json{
            {"main_camera", s.main_camera},
            {"directional_light", s.directional_light},
            {"ambient_strength", s.ambient_strength},
            {"entities", s.entities}
        };
    }
    inline void from_json(const nlohmann::json& j, Scene& s) {
        j.at("main_camera").get_to(s.main_camera);
        j.at("directional_light").get_to(s.directional_light);
        j.at("ambient_strength").get_to(s.ambient_strength);
        j.at("entities").get_to(s.entities);
    }
}
