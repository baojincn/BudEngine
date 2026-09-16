#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include "src/core/bud.math.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/robots/bud.robot.loader.hpp"
#include "src/runtime/bud.scene.hpp"
#include <glm/gtc/matrix_transform.hpp>

namespace bud::engine {
    class BudEngine;
}

namespace bud::robots {

struct VisualPartEntry {
    std::string link_name;
    size_t entity_index = 0;
    bud::math::mat4 local_offset{ 1.0f };
};

class RobotVisualBridge {
public:
    RobotVisualBridge() = default;
    ~RobotVisualBridge() = default;

    // Initializes visual entities in the scene for each link with visual geometry
    bool init(bud::engine::BudEngine* engine,
              bud::scene::Scene& scene,
              const bud::robots::RobotDef& robot_def,
              const std::string& package_root);

    // Synchronizes the entity transforms from the simulated robot instance
    void sync_transforms(const bud::robots::RobotInstance& robot,
                         bud::scene::Scene& scene);

    // Synchronizes the entity transforms directly from world link transforms (exact rigid FK hierarchy)
    void sync_transforms(const std::unordered_map<std::string, glm::mat4>& world_link_transforms,
                         bud::scene::Scene& scene);

    // Shows or hides all robot visual parts
    void set_visible(bool visible, bud::scene::Scene& scene);

    size_t get_visual_parts_count() const {
        return m_parts.size();
    }

    bool is_initialized() const {
        return m_initialized;
    }

    bool is_visible() const {
        return m_visible;
    }

    // Declares that link transforms from get_all_link_transforms() are in engine Y-up space
    // (returned by the MuJoCo backend via from_mujoco conversion), while visual local offsets
    // are in URDF Z-up space. The basis change is applied in sync_transforms to reconcile them.
    void set_simulation_mode(bool is_sim) { m_is_simulation = is_sim; }

private:
    std::vector<VisualPartEntry> m_parts;
    std::unordered_map<std::string, std::vector<size_t>> m_link_to_part_indices;
    bool m_initialized = false;
    bool m_visible = true;
    // True when the backend is MuJoCo: link world transforms come in engine Y-up space
    // but visual offsets are authored in URDF Z-up space, so a basis conversion is needed.
    bool m_is_simulation = false;
};

} // namespace bud::robots
