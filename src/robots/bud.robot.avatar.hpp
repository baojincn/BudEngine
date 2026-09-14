#pragma once

#include <memory>
#include <string>
#include <vector>
#include "src/core/bud.math.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/robots/bud.robot.loader.hpp"
#include "src/robots/bud.robot.visual_bridge.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/runtime/bud.character_controller.hpp"
#include "src/input/bud.input.manager.hpp"
#include <unordered_map>

namespace bud::engine {
    class BudEngine;
}

namespace bud::graphics {
    struct PhysicsDebugVertex;
}

namespace bud::robots {

enum class AvatarCameraView {
    ThirdPerson,
    FirstPerson
};

enum class AvatarRenderMode {
    ArticulatedRigid  // Scheme B: Articulated multi-link rigid hierarchy via Jolt physics
};

class RobotAvatarController {
public:
    RobotAvatarController();
    ~RobotAvatarController();

    bool init(bud::engine::BudEngine* engine,
              const std::string& robot_file = "Content/Robots/g1_description/g1_29dof.budasset",
              const std::string& package_root = "Content/Robots/g1_description");

    void update(float dt, const bud::input::Input& input, bud::scene::Camera& camera);

    void toggle_camera_mode(bud::scene::Camera& camera);
    void set_camera_view(AvatarCameraView view, bud::scene::Camera& camera);
    AvatarCameraView get_camera_view() const {
        return m_view_mode;
    }

    void toggle_render_mode() {}
    void set_render_mode(AvatarRenderMode mode) {
        m_render_mode = mode;
    }
    AvatarRenderMode get_render_mode() const {
        return m_render_mode;
    }

    bud::math::vec3 get_head_camera_position() const;
    bud::math::vec3 get_pelvis_position() const;
    bud::math::vec3 get_torso_position() const;
    float get_ground_height(const bud::math::vec3& test_pos) const;
    bool get_ground_height(const bud::math::vec3& test_pos, float& out_height) const;

    bud::robots::RobotInstance* get_robot() const {
        return m_robot.get();
    }

    bud::robots::RobotVisualBridge* get_visual_bridge() const {
        return m_visual_bridge.get();
    }

    void get_debug_collision_vertices(std::vector<bud::graphics::PhysicsDebugVertex>& out_verts) const;

private:
    void apply_walking_gait(float dt, float speed);
    void reset_to_idle_stance();
    void update_forward_kinematics();

    bud::engine::BudEngine* m_engine = nullptr;
    std::unique_ptr<bud::robots::RobotInstance> m_robot;
    std::unique_ptr<bud::robots::RobotVisualBridge> m_visual_bridge;

    AvatarCameraView m_view_mode = AvatarCameraView::ThirdPerson;
    AvatarRenderMode m_render_mode = AvatarRenderMode::ArticulatedRigid;

    float m_current_yaw = 0.0f;
    float m_gait_phase = 0.0f;
    bool m_is_moving = false;

    bud::math::vec3 m_current_pelvis_pos{ 0.0f, 0.85f, 0.0f };
    std::vector<uint32_t> m_ignored_body_ids;

    bud::math::quaternion m_urdf_to_world_rot{ 1.0f, 0.0f, 0.0f, 0.0f };

    std::unordered_map<std::string, float> m_target_joint_angles;
    std::unordered_map<std::string, float> m_current_joint_angles;
    std::unordered_map<std::string, glm::mat4> m_current_link_xforms;
};

} // namespace bud::robots
