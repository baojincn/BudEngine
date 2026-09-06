#include <fstream>
#include <filesystem>
#include <algorithm>
#include "src/runtime/bud.scene.builder.hpp"
#include "src/runtime/bud.scene.io.hpp"

namespace bud::scene
{
    Scene SceneBuilder::create_scene(const SceneTemplateConfig& config)
    {
        Scene scene;
        scene.ambient_strength = config.ambient_strength;
        scene.lod_error_threshold_px = config.lod_error_threshold_px;
        scene.streaming_unload_radius = config.streaming_unload_radius;

        scene.main_camera.position = config.camera_position;
        scene.main_camera.yaw = config.camera_yaw;
        scene.main_camera.pitch = config.camera_pitch;
        scene.main_camera.zoom = config.camera_zoom;
        scene.main_camera.movement_speed = config.camera_speed;
        scene.main_camera.mouse_sensitivity = config.camera_sensitivity;
        scene.main_camera.rebuild_camera_vectors();

        scene.directional_light.direction = config.sun_direction;
        scene.directional_light.color = config.sun_color;
        scene.directional_light.intensity = config.sun_intensity;

        return scene;
    }

    Scene SceneBuilder::create_empty_scene(const std::string& name)
    {
        SceneTemplateConfig config;
        config.scene_name = name;
        return create_scene(config);
    }

    bool SceneBuilder::load_scene_from_file(const std::string& filepath, Scene& out_scene)
    {
        if (!std::filesystem::exists(filepath))
            return false;

        std::ifstream in(filepath);
        if (!in.is_open())
            return false;

        try
        {
            nlohmann::json j;
            in >> j;
            out_scene = j.get<Scene>();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool SceneBuilder::save_scene_to_file(const Scene& scene, const std::string& filepath)
    {
        std::error_code ec;
        std::filesystem::path p(filepath);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);

        std::ofstream out(filepath);
        if (!out.is_open())
            return false;

        nlohmann::ordered_json j = scene;
        out << dump_scene(j);
        return true;
    }

    uint32_t SceneBuilder::add_entity(Scene& scene, const Entity& entity)
    {
        scene.entities.push_back(entity);
        return static_cast<uint32_t>(scene.entities.size() - 1);
    }

    bool SceneBuilder::remove_entity_by_index(Scene& scene, size_t index)
    {
        if (index >= scene.entities.size())
            return false;

        scene.entities.erase(scene.entities.begin() + index);
        return true;
    }

    bool SceneBuilder::remove_entity_by_name(Scene& scene, const std::string& name)
    {
        auto it = std::find_if(scene.entities.begin(), scene.entities.end(), [&](const Entity& e)
        {
            return e.name == name;
        });

        if (it == scene.entities.end())
            return false;

        scene.entities.erase(it);
        return true;
    }

    void SceneBuilder::append_entities(Scene& target_scene, const std::vector<Entity>& entities)
    {
        target_scene.entities.insert(target_scene.entities.end(), entities.begin(), entities.end());
    }

    void SceneBuilder::merge_scene(Scene& base_scene, const Scene& incoming_scene)
    {
        append_entities(base_scene, incoming_scene.entities);
    }
}
