#pragma once

#include <string>
#include <vector>
#include "src/runtime/bud.scene.hpp"

namespace bud::scene
{
    struct SceneTemplateConfig
    {
        std::string scene_name = "new scene";
        bud::math::vec3 camera_position = { 0.0f, 1.5f, 3.0f };
        float camera_yaw = 0.0f;
        float camera_pitch = 0.0f;
        float camera_zoom = 45.0f;
        float camera_speed = 5.0f;
        float camera_sensitivity = 0.1f;
        bud::math::vec3 sun_direction = { 100.0f, 250.0f, 80.0f };
        bud::math::vec3 sun_color = { 1.0f, 0.95f, 0.85f };
        float sun_intensity = 1.2f;
        float ambient_strength = 0.25f;
        float lod_error_threshold_px = 2.0f;
        float streaming_unload_radius = 500.0f;
    };

    class SceneBuilder
    {
    public:
        static Scene create_scene(const SceneTemplateConfig& config = {});
        static Scene create_empty_scene(const std::string& name = "Empty Scene");

        static bool load_scene_from_file(const std::string& filepath, Scene& out_scene);
        static bool save_scene_to_file(const Scene& scene, const std::string& filepath);

        static uint32_t add_entity(Scene& scene, const Entity& entity);
        static bool remove_entity_by_index(Scene& scene, size_t index);
        static bool remove_entity_by_name(Scene& scene, const std::string& name);
        static void append_entities(Scene& target_scene, const std::vector<Entity>& entities);
        static void merge_scene(Scene& base_scene, const Scene& incoming_scene);
    };
}
