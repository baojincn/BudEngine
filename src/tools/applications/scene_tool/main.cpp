#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <glm/gtc/matrix_transform.hpp>
#include "src/runtime/bud.scene.builder.hpp"
#include "src/runtime/bud.scene.io.hpp"

namespace
{
    void print_usage()
    {
        std::cout << "Usage: SceneTool [command] [options]" << std::endl;
        std::cout << "Commands:" << std::endl;
        std::cout << "  --create                  Create a new scene file" << std::endl;
        std::cout << "  --info                    Inspect an existing scene file" << std::endl;
        std::cout << "  --add-entity              Add an entity to an existing scene" << std::endl;
        std::cout << "  --remove-entity           Remove an entity from a scene" << std::endl;
        std::cout << "  --merge                   Merge a source scene into target scene" << std::endl;
        std::cout << "\nOptions:" << std::endl;
        std::cout << "  --name <path_or_name>     Scene path (relative/absolute) or name (extensions: .budscene / .json, default: .json)" << std::endl;
        std::cout << "  --template <type>         Scene template: default (default), studio, empty" << std::endl;
        std::cout << "  --asset <path>            Asset path (.budasset) for --add-entity" << std::endl;
        std::cout << "  --pos <x,y,z>             Position for --add-entity (default: 0,0,0)" << std::endl;
        std::cout << "  --scale <x,y,z or s>      Scale for --add-entity (default: 1,1,1)" << std::endl;
        std::cout << "  --render-type <type>      Render type: VirtualGeometry (default), Translucent, Dynamic" << std::endl;
        std::cout << "  --entity-name <name>      Entity name to remove (for --remove-entity)" << std::endl;
        std::cout << "  --entity-index <idx>      Entity index to remove (for --remove-entity)" << std::endl;
        std::cout << "  --source-scene <path>     Source scene to merge from (for --merge)" << std::endl;
        std::cout << "  --target-scene <path>     Target scene to merge into (for --merge)" << std::endl;
        std::cout << "  --help, -h                Show help" << std::endl;
    }

    bool parse_vec3(const std::string& str, bud::math::vec3& out_vec)
    {
        std::stringstream ss(str);
        std::string item;
        std::vector<float> values;
        while (std::getline(ss, item, ','))
        {
            try
            {
                values.push_back(std::stof(item));
            }
            catch (...)
            {
                return false;
            }
        }

        if (values.size() == 1)
        {
            out_vec = bud::math::vec3(values[0]);
            return true;
        }

        if (values.size() == 3)
        {
            out_vec = bud::math::vec3(values[0], values[1], values[2]);
            return true;
        }

        return false;
    }

    std::string resolve_scene_target_path(const std::string& input_name)
    {
        if (input_name.empty())
            return "Content/new scene.json";

        std::filesystem::path p(input_name);
        if (p.is_absolute())
        {
            if (!p.has_extension())
                return (input_name + ".json");
            return input_name;
        }

        if (p.has_parent_path())
        {
            if (!p.has_extension())
                return (input_name + ".json");
            return p.generic_string();
        }

        std::string fname = p.has_extension() ? input_name : (input_name + ".json");
        return (std::filesystem::path("Content") / fname).generic_string();
    }

    std::string resolve_scene_input_path(const std::string& input_path)
    {
        if (input_path.empty())
            return input_path;
        if (std::filesystem::exists(input_path))
            return input_path;

        std::filesystem::path p(input_path);
        if (!p.has_extension())
        {
            if (std::filesystem::exists(input_path + ".json"))
                return input_path + ".json";
            if (std::filesystem::exists(input_path + ".budscene"))
                return input_path + ".budscene";
        }

        if (!p.has_parent_path())
        {
            std::vector<std::string> candidates;
            if (p.has_extension())
            {
                candidates.push_back((std::filesystem::path("Content") / input_path).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / input_path).generic_string());
            }
            else
            {
                candidates.push_back((std::filesystem::path("Content") / (input_path + ".json")).generic_string());
                candidates.push_back((std::filesystem::path("Content") / (input_path + ".budscene")).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / (input_path + ".json")).generic_string());
                candidates.push_back((std::filesystem::path("Content/Scenes") / (input_path + ".budscene")).generic_string());
                candidates.push_back((std::filesystem::path("Content") / input_path).generic_string());
            }

            for (const auto& cand : candidates)
            {
                if (std::filesystem::exists(cand))
                    return cand;
            }

            return (std::filesystem::path("Content") / (p.has_extension() ? input_path : (input_path + ".json"))).generic_string();
        }

        return input_path;
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        print_usage();
        return 0;
    }

    std::string command;
    std::string scene_path;
    std::string output_path;
    std::string source_scene_path;
    std::string target_scene_path;
    std::string name;
    std::string template_type = "default";
    std::string asset_path;
    std::string pos_str = "0,0,0";
    std::string scale_str = "1,1,1";
    std::string entity_name_to_remove;
    int entity_index_to_remove = -1;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--create" || arg == "--info" || arg == "--add-entity" || arg == "--remove-entity" || arg == "--merge")
        {
            command = arg;
        }
        else if (arg == "--output" && i + 1 < argc)
        {
            output_path = argv[++i];
        }
        else if (arg == "--scene" && i + 1 < argc)
        {
            scene_path = argv[++i];
        }
        else if (arg == "--source-scene" && i + 1 < argc)
        {
            source_scene_path = argv[++i];
        }
        else if (arg == "--target-scene" && i + 1 < argc)
        {
            target_scene_path = argv[++i];
        }
        else if (arg == "--name" && i + 1 < argc)
        {
            name = argv[++i];
        }
        else if (arg == "--template" && i + 1 < argc)
        {
            template_type = argv[++i];
        }
        else if (arg == "--asset" && i + 1 < argc)
        {
            asset_path = argv[++i];
        }
        else if (arg == "--pos" && i + 1 < argc)
        {
            pos_str = argv[++i];
        }
        else if (arg == "--scale" && i + 1 < argc)
        {
            scale_str = argv[++i];
        }
        else if (arg == "--entity-name" && i + 1 < argc)
        {
            entity_name_to_remove = argv[++i];
        }
        else if (arg == "--entity-index" && i + 1 < argc)
        {
            entity_index_to_remove = std::stoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-h")
        {
            print_usage();
            return 0;
        }
    }

    if (command == "--create")
    {
        std::string target_raw = !name.empty() ? name : (!output_path.empty() ? output_path : scene_path);
        std::string target_file = resolve_scene_target_path(target_raw);

        bud::scene::SceneTemplateConfig config;
        config.scene_name = std::filesystem::path(target_file).stem().string();

        if (template_type == "empty")
        {
            config.sun_intensity = 0.0f;
            config.ambient_strength = 0.0f;
        }
        else if (template_type == "studio")
        {
            config.camera_position = { 0.0f, 1.0f, 3.5f };
            config.sun_direction = { 0.0f, 100.0f, 50.0f };
            config.sun_color = { 1.0f, 1.0f, 1.0f };
            config.sun_intensity = 1.0f;
            config.ambient_strength = 0.35f;
        }
        else // default / outdoor
        {
            config.camera_position = { 0.0f, 1.5f, 5.0f };
            config.sun_direction = { 100.0f, 250.0f, 80.0f };
            config.sun_color = { 1.0f, 0.95f, 0.85f };
            config.sun_intensity = 1.2f;
            config.ambient_strength = 0.25f;
            config.lod_error_threshold_px = 2.0f;
            config.streaming_unload_radius = 500.0f;
        }

        bud::scene::Scene scene = bud::scene::SceneBuilder::create_scene(config);
        if (bud::scene::SceneBuilder::save_scene_to_file(scene, target_file))
        {
            std::cout << "[SceneTool] Successfully created scene: " << target_file << " (template: " << template_type << ")" << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "[SceneTool] Failed to save scene to: " << target_file << std::endl;
            return 1;
        }
    }
    else if (command == "--info")
    {
        std::string target_file = !name.empty() ? name : scene_path;
        if (target_file.empty())
        {
            std::cerr << "[SceneTool] Error: --name <path> or --scene <path> is required for --info." << std::endl;
            return 1;
        }

        std::string resolved_scene = resolve_scene_input_path(target_file);
        bud::scene::Scene scene;
        if (!bud::scene::SceneBuilder::load_scene_from_file(resolved_scene, scene))
        {
            std::cerr << "[SceneTool] Failed to load scene from: " << resolved_scene << std::endl;
            return 1;
        }

        std::cout << "================ Scene Info: " << resolved_scene << " ================" << std::endl;
        std::cout << "Ambient Strength:         " << scene.ambient_strength << std::endl;
        std::cout << "LOD Error Threshold (px): " << scene.lod_error_threshold_px << std::endl;
        std::cout << "Streaming Unload Radius:  " << scene.streaming_unload_radius << " m" << std::endl;
        std::cout << "Camera Pos:               (" << scene.main_camera.position.x << ", "
                  << scene.main_camera.position.y << ", " << scene.main_camera.position.z << ")" << std::endl;
        std::cout << "Sun Direction:            (" << scene.directional_light.direction.x << ", "
                  << scene.directional_light.direction.y << ", " << scene.directional_light.direction.z << ")" << std::endl;
        std::cout << "Sun Intensity:            " << scene.directional_light.intensity << std::endl;
        std::cout << "Total Entities:           " << scene.entities.size() << std::endl;

        std::cout << "\nEntities Summary:" << std::endl;
        for (size_t i = 0; i < scene.entities.size(); ++i)
        {
            const auto& e = scene.entities[i];
            std::cout << "  [" << i << "] Name: " << (e.name.empty() ? "(unnamed)" : e.name)
                      << " | Asset: " << e.asset_path << std::endl;
            if (i >= 20 && scene.entities.size() > 25)
            {
                std::cout << "  ... and " << (scene.entities.size() - i - 1) << " more entities." << std::endl;
                break;
            }
        }
        return 0;
    }
    else if (command == "--add-entity")
    {
        std::string target_file = !scene_path.empty() ? scene_path : name;
        if (target_file.empty())
        {
            std::cerr << "[SceneTool] Error: --scene <path> is required for --add-entity." << std::endl;
            return 1;
        }
        if (asset_path.empty())
        {
            std::cerr << "[SceneTool] Error: --asset <path> is required for --add-entity." << std::endl;
            return 1;
        }

        std::string resolved_scene = resolve_scene_input_path(target_file);
        bud::scene::Scene scene;
        if (!bud::scene::SceneBuilder::load_scene_from_file(resolved_scene, scene))
        {
            std::cerr << "[SceneTool] Failed to load scene from: " << resolved_scene << std::endl;
            return 1;
        }

        bud::math::vec3 pos(0.0f);
        parse_vec3(pos_str, pos);

        bud::math::vec3 scale(1.0f);
        parse_vec3(scale_str, scale);

        bud::scene::Entity entity;
        entity.name = name.empty() ? std::filesystem::path(asset_path).stem().string() : name;
        entity.asset_path = asset_path;
        entity.is_active = true;
        entity.is_static = true;
        entity.mesh_index = 0;
        entity.material_index = 0;

        bud::math::mat4 tf(1.0f);
        tf = glm::translate(tf, pos);
        tf = glm::scale(tf, scale);
        entity.transform = tf;

        uint32_t idx = bud::scene::SceneBuilder::add_entity(scene, entity);
        if (bud::scene::SceneBuilder::save_scene_to_file(scene, resolved_scene))
        {
            std::cout << "[SceneTool] Successfully added entity [" << idx << "] '" << entity.name
                      << "' to scene: " << resolved_scene << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "[SceneTool] Failed to save updated scene to: " << resolved_scene << std::endl;
            return 1;
        }
    }
    else if (command == "--remove-entity")
    {
        std::string target_file = !scene_path.empty() ? scene_path : name;
        if (target_file.empty())
        {
            std::cerr << "[SceneTool] Error: --scene <path> is required for --remove-entity." << std::endl;
            return 1;
        }

        std::string resolved_scene = resolve_scene_input_path(target_file);
        bud::scene::Scene scene;
        if (!bud::scene::SceneBuilder::load_scene_from_file(resolved_scene, scene))
        {
            std::cerr << "[SceneTool] Failed to load scene from: " << resolved_scene << std::endl;
            return 1;
        }

        bool ok = false;
        if (entity_index_to_remove >= 0)
        {
            ok = bud::scene::SceneBuilder::remove_entity_by_index(scene, static_cast<size_t>(entity_index_to_remove));
        }
        else if (!entity_name_to_remove.empty())
        {
            ok = bud::scene::SceneBuilder::remove_entity_by_name(scene, entity_name_to_remove);
        }
        else
        {
            std::cerr << "[SceneTool] Error: Specify --entity-name <name> or --entity-index <idx> to remove." << std::endl;
            return 1;
        }

        if (ok)
        {
            bud::scene::SceneBuilder::save_scene_to_file(scene, resolved_scene);
            std::cout << "[SceneTool] Successfully removed entity from scene: " << resolved_scene << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "[SceneTool] Entity not found in scene." << std::endl;
            return 1;
        }
    }
    else if (command == "--merge")
    {
        if (target_scene_path.empty() && !scene_path.empty())
            target_scene_path = scene_path;

        if (target_scene_path.empty() || source_scene_path.empty())
        {
            std::cerr << "[SceneTool] Error: --target-scene <path> and --source-scene <path> are required for --merge." << std::endl;
            return 1;
        }

        std::string resolved_target = resolve_scene_input_path(target_scene_path);
        std::string resolved_source = resolve_scene_input_path(source_scene_path);

        bud::scene::Scene target_scene;
        if (!bud::scene::SceneBuilder::load_scene_from_file(resolved_target, target_scene))
        {
            std::cerr << "[SceneTool] Failed to load target scene: " << resolved_target << std::endl;
            return 1;
        }

        bud::scene::Scene source_scene;
        if (!bud::scene::SceneBuilder::load_scene_from_file(resolved_source, source_scene))
        {
            std::cerr << "[SceneTool] Failed to load source scene: " << resolved_source << std::endl;
            return 1;
        }

        size_t added_count = source_scene.entities.size();
        bud::scene::SceneBuilder::merge_scene(target_scene, source_scene);

        if (bud::scene::SceneBuilder::save_scene_to_file(target_scene, resolved_target))
        {
            std::cout << "[SceneTool] Successfully merged " << added_count << " entities from '"
                      << resolved_source << "' into '" << resolved_target << "'." << std::endl;
            return 0;
        }
        else
        {
            std::cerr << "[SceneTool] Failed to save merged scene: " << resolved_target << std::endl;
            return 1;
        }
    }
    else
    {
        std::cerr << "[SceneTool] Unknown command: " << command << std::endl;
        print_usage();
        return 1;
    }
}
