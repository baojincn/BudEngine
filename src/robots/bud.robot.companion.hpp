#pragma once

#include <memory>
#include <string>
#include <glm/glm.hpp>

#include "src/core/bud.math.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/robots/bud.robot.loader.hpp"
#include "src/robots/bud.robot.visual_bridge.hpp"
#include "src/rl/bud.rl.microduck_policy_controller.hpp"

namespace bud::engine {
    class BudEngine;
}

namespace bud::robots {

class MicroduckCompanionController {
public:
    MicroduckCompanionController();
    ~MicroduckCompanionController();

    bool init(
        bud::engine::BudEngine* engine,
        const bud::math::vec3& spawn_offset = bud::math::vec3(0.0f, 0.20f, -1.0f),
        const std::string& robot_asset_path = "Content/Robots/microduck/microduck.budasset",
        const std::string& package_root = "Content/Robots/microduck"
    );

    // Call this after prepare_simulation() (i.e. after the physics world has been compiled with all
    // articulations present) to attach the ONNX policy. Separated from init() so the first
    // set_articulation_joint_commands does not trigger a recompile of an already-built model.
    bool start_policy(const std::string& onnx_policy_path = "Content/rl/microduck/velstand.onnx");

    void update_follower(float dt, const bud::math::vec3& g1_pelvis_pos, const bud::math::vec3& g1_forward);

    bool is_initialized() const { return initialized; }
    bool is_policy_active() const { return policy_controller != nullptr; }
    const bud::math::vec3& get_position() const { return current_position; }
    RobotInstance* get_robot() const { return robot.get(); }
    RobotVisualBridge* get_visual_bridge() const { return visual_bridge.get(); }

private:
    bud::engine::BudEngine* engine_ptr = nullptr;
    std::unique_ptr<RobotInstance> robot;
    std::unique_ptr<RobotVisualBridge> visual_bridge;
    std::unique_ptr<rl::MicroduckPolicyController> policy_controller;

    bud::math::vec3 current_position{ 0.0f };
    float cmd_vx = 0.0f;
    float cmd_vyaw = 0.0f;
    float fallen_timer = 0.0f;
    bool initialized = false;

};

} // namespace bud::robots
