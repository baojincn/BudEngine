#include "src/robots/bud.robot.avatar.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"
#include "src/runtime/bud.engine.hpp"
#include "src/physics/bud.physics.scene.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>

#include <iostream>
#include <cmath>
#include <algorithm>
#include <queue>
#include <unordered_set>

namespace bud::robots {

namespace {

constexpr float k_gait_frequency = 6.2831853f * 1.6f; // ~1.6 Hz humanoid stride
constexpr float k_max_gait_phase = 6.2831853f * 100.0f;
constexpr float k_hip_amplitude = 0.45f;              // ~25 degrees
constexpr float k_knee_amplitude = 0.55f;             // ~31 degrees
constexpr float k_arm_amplitude = 0.35f;              // ~20 degrees
constexpr float k_walk_speed = 2.0f;
constexpr float k_idle_hip_pitch = -0.15f;
constexpr float k_idle_knee_angle = 0.30f;
constexpr float k_idle_ankle_pitch = -0.15f;
constexpr float k_idle_elbow_angle = 0.6f;

// G1 Humanoid Dimensions (exact from cooked g1.budasset: AABB min_y = -0.792f, max_y = 0.530573m)
constexpr float k_mesh_foot_sole_offset_y = -0.792f;
constexpr float k_torso_relative_y = 0.20f;
constexpr float k_head_relative_y = 0.42f;
constexpr float k_head_fwd_offset = 0.12f;
constexpr float k_default_orbit_dist = 1.8f;
constexpr float k_default_orbit_pitch = -10.0f;
constexpr float k_default_orbit_yaw = 180.0f;

} // namespace

RobotAvatarController::RobotAvatarController() {
    // Basis mapping URDF standard (+X fwd, +Y left, +Z up) to BudEngine (+Y up, -Z fwd, -X left)
    glm::mat3 urdf_basis(
        glm::vec3(0.0f, 0.0f, -1.0f),
        glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    m_urdf_to_world_rot = glm::quat_cast(urdf_basis);
}

RobotAvatarController::~RobotAvatarController() {
    if (m_engine && m_engine->get_robot_avatar() == this)
        m_engine->set_robot_avatar(nullptr);
}

bool RobotAvatarController::get_ground_height(const bud::math::vec3& test_pos, float& out_height) const {
    if (!m_engine)
        return false;

    auto* physics_scene = m_engine->get_physics_scene();
    if (!physics_scene)
        return false;

    auto* controller = m_engine->get_character_controller();
    const float capsule_offset = controller ? (controller->get_capsule_height() * 0.5f + controller->get_capsule_radius()) : 0.80f;
    const float feet_est_y = test_pos.y - capsule_offset;

    if (physics_scene->get_backend() == physics::PhysicsBackend::Mujoco) {
        const bud::math::vec3 ray_start(test_pos.x, std::max(test_pos.y + 1.0f, 2.0f), test_pos.z);
        const bud::math::vec3 ray_end(test_pos.x, -10.0f, test_pos.z);
        const auto hit = physics_scene->raycast(ray_start, ray_end);
        if (hit.has_value() && hit->hit) {
            out_height = hit->hit_point.y;
            return true;
        }
        out_height = physics_scene->get_ground_plane_height();
        return true;
    }

    auto* system = physics_scene->get_jolt_system();
    if (!system)
        return false;

    // Raycast strictly downwards starting from just above the feet to avoid overhead arches/ceilings
    JPH::RRayCast ray(
        JPH::RVec3(test_pos.x, feet_est_y + 0.25f, test_pos.z),
        JPH::Vec3(0.0f, -0.60f, 0.0f)
    );
    JPH::RayCastResult hit;

    class GroundFilter : public JPH::BodyFilter {
    public:
        const std::vector<uint32_t>* ignored = nullptr;
        bool ShouldCollide(const JPH::BodyID& id) const override {
            if (ignored && !ignored->empty()) {
                uint32_t raw_id = id.GetIndexAndSequenceNumber();
                for (uint32_t ign : *ignored) {
                    if (ign == raw_id)
                        return false;
                }
            }
            return true;
        }
    };

    GroundFilter filter;
    filter.ignored = &m_ignored_body_ids;

    if (system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) {
        out_height = static_cast<float>(ray.mOrigin.GetY() + hit.mFraction * ray.mDirection.GetY());
        return true;
    }

    return false;
}

float RobotAvatarController::get_ground_height(const bud::math::vec3& test_pos) const {
    float h = 0.0f;
    if (get_ground_height(test_pos, h))
        return h;
    return 0.0f;
}

bool RobotAvatarController::init(bud::engine::BudEngine* engine,
                                 const std::string& robot_file,
                                 const std::string& package_root) {
    if (!engine)
        return false;

    m_engine = engine;
    if (m_engine)
        m_engine->set_robot_avatar(this);

    auto* physics_scene = engine->get_physics_scene();
    if (!physics_scene) {
        std::cerr << "[RobotAvatarController] PhysicsScene is null in BudEngine." << std::endl;
        return false;
    }

    auto* controller = engine->get_character_controller();
    bud::math::vec3 spawn_pos{ 0.0f, 1.8f, 0.0f };
    if (controller && physics_scene->get_backend() != physics::PhysicsBackend::Mujoco)
        spawn_pos = controller->get_position();

    float capsule_ground_offset = controller ? (controller->get_capsule_height() * 0.5f + controller->get_capsule_radius()) : 0.80f;

    float ground_y = spawn_pos.y - capsule_ground_offset;
    get_ground_height(spawn_pos, ground_y);

    // Pelvis position relative to ground: soles start a little above the surface and
    // settle onto it through physics. Spawning perfectly flush used to sweep the feet
    // into the collision mesh on the very first frames.
    constexpr float kSpawnClearanceY = 0.03f;
    m_current_pelvis_pos = spawn_pos;
    if (physics_scene->get_backend() == physics::PhysicsBackend::Mujoco) {
        m_current_pelvis_pos.x = 0.0f;
        m_current_pelvis_pos.z = 0.0f;
        m_current_pelvis_pos.y = ground_y + k_g1_standing_pelvis_height;
    }
    else {
        m_current_pelvis_pos.y = ground_y - k_mesh_foot_sole_offset_y + kSpawnClearanceY;
    }

    RobotSpawnParams params{};
    params.position = m_current_pelvis_pos;
    if (physics_scene->get_backend() == physics::PhysicsBackend::Mujoco) {
        // MuJoCo simulation basis: URDF (+X fwd, +Y left, +Z up) -> Engine (+X fwd, -Z left, +Y up)
        glm::mat3 urdf_basis(
            glm::vec3(1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, -1.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        m_urdf_to_world_rot = glm::quat_cast(urdf_basis);
        params.rotation = bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    }
    else {
        params.rotation = m_urdf_to_world_rot;
    }
    params.activate = true;
    params.enable_motors = true;
    params.default_motor_stiffness = 800.0f;
    params.default_motor_damping = 80.0f;
    // The pelvis is kinematic and the joints are position motors, so a strong motor
    // presses a blocked leg straight through the floor mesh (deep convex-vs-mesh
    // penetration is what kills Jolt's EPA in release builds). A moderate torque cap
    // lets the foot be stopped by the ground instead of tunnelling into it.
    params.default_motor_max_torque = 150.0f;
    params.asset_path = robot_file;
    if (physics_scene->get_backend() == physics::PhysicsBackend::Mujoco)
        params.initial_joint_angles = get_g1_standing_joint_angles();

    m_robot = RobotLoader::spawn_robot_from_file(*physics_scene, robot_file, params);
    if (m_robot) {
        reset_to_idle_stance();
        m_current_joint_angles = m_target_joint_angles;
        if (m_robot->is_simulation()) {
            m_robot->set_low_cmd(make_g1_standing_cmd());
        } else if (m_robot->get_ragdoll()) {
            m_ignored_body_ids.clear();
            for (int i = 0; i < m_robot->get_ragdoll()->GetBodyCount(); ++i) {
                m_ignored_body_ids.push_back(m_robot->get_ragdoll()->GetBodyID(i).GetIndexAndSequenceNumber());
            }
            if (controller) {
                for (uint32_t id : m_ignored_body_ids) {
                    controller->add_ignored_body(id);
                }
            }
        }
    }

    auto& scene = engine->get_scene();
    m_visual_bridge = std::make_unique<RobotVisualBridge>();
    if (m_robot) {
        m_visual_bridge->init(engine, scene, m_robot->get_definition(), package_root);
    }

    std::cout << "[RobotAvatarController] Initialized Unitree G1 Avatar (Pelvis Y: "
              << m_current_pelvis_pos.y << ", Spawn Y: " << spawn_pos.y << ")." << std::endl;
    return true;
}

bool RobotAvatarController::load_policy(const std::string& onnx_path, const std::string& spec_path,
                                        std::string& error) {
    if (!m_robot || !m_robot->is_simulation()) {
        error = "a policy can only drive a simulated (MuJoCo) robot";
        return false;
    }
    auto* physics_scene = m_engine ? m_engine->get_physics_scene() : nullptr;
    if (!physics_scene) {
        error = "no physics scene to load a policy into";
        return false;
    }

    std::cout << "[RobotAvatarController] loading policy onnx='" << onnx_path << "' spec='" << spec_path
              << "'" << std::endl;

    bud::rl::PolicyControllerConfig config;
    config.enabled = true;
    config.onnx_path = onnx_path;
    config.spec_path = spec_path;

    // Loading touches external files (spec JSON, ONNX graph) and ONNX Runtime, any of which can
    // throw. This runs on the async scene-load callback thread, where an escaping exception would
    // terminate the process, so report it as a load failure instead.
    auto controller = std::make_unique<bud::rl::G1PolicyController>();
    try {
        if (!controller->initialize(physics_scene->get_world(), m_robot->get_articulation_handle(),
                                    m_robot->get_definition(), config, error)) {
            m_policy_error = error;
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("policy load threw: ") + e.what();
        m_policy_error = error;
        return false;
    } catch (...) {
        error = "policy load threw an unknown exception";
        m_policy_error = error;
        return false;
    }

    m_policy = std::move(controller);
    m_policy_error.clear();
    std::cout << "[RobotAvatarController] policy '" << m_policy->spec().name << "' loaded: obs="
              << m_policy->observation_dim() << " action=" << m_policy->action_dim()
              << (m_policy->has_network() ? "" : " (null policy: zero action, holds the default pose)")
              << std::endl;
    return true;
}

bool RobotAvatarController::has_policy() const {
    return m_policy && m_policy->ready();
}

const std::string& RobotAvatarController::policy_error() const {
    return m_policy_error;
}

void RobotAvatarController::set_policy_command(const bud::math::vec3& command) {
    m_policy_command_override = command;
    m_has_policy_command_override = true;
}

bud::math::vec3 RobotAvatarController::policy_command_from_input(const bud::input::Input& input) const {
    // Command convention: (vx forward, vy left, yaw_rate counter-clockwise).
    float vx = 0.0f;
    float vy = 0.0f;
    float yaw_rate = 0.0f;

    // Keyboard bindings:
    // W / S: Forward / Backward along robot heading
    // A / D: Steer Left / Right (yaw rate)
    // Q / E: Lateral Strafe Left / Right
    if (input.is_key_down(bud::input::Key::W))
        vx += 0.8f;
    if (input.is_key_down(bud::input::Key::S))
        vx -= 0.4f;
    if (input.is_key_down(bud::input::Key::A))
        yaw_rate += 0.5f;
    if (input.is_key_down(bud::input::Key::D))
        yaw_rate -= 0.5f;
    if (input.is_key_down(bud::input::Key::Q))
        vy += 0.25f;
    if (input.is_key_down(bud::input::Key::E))
        vy -= 0.25f;

    // Gamepad bindings:
    // Left Stick: Y = Forward/Backward, X = Steer Left/Right
    // LB / RB & D-Pad Left/Right: Lateral Strafe Left/Right
    // D-Pad Up/Down: Forward/Backward
    if (input.is_gamepad_connected()) {
        const auto deadzone = [](float value) {
            constexpr float kDeadzone = 0.15f;
            if (std::abs(value) < kDeadzone)
                return 0.0f;
            return (value - std::copysign(kDeadzone, value)) / (1.0f - kDeadzone);
        };
        const float ly = -deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftY));
        const float lx = deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftX));

        if (std::abs(ly) > 1.0e-3f)
            vx += (ly > 0.0f) ? (ly * 0.8f) : (ly * 0.4f);
        if (std::abs(lx) > 1.0e-3f)
            yaw_rate += -lx * 0.5f;

        if (input.is_gamepad_button_down(bud::input::GamepadButton::LB) ||
            input.is_gamepad_button_down(bud::input::GamepadButton::DPadLeft))
            vy += 0.25f;
        if (input.is_gamepad_button_down(bud::input::GamepadButton::RB) ||
            input.is_gamepad_button_down(bud::input::GamepadButton::DPadRight))
            vy -= 0.25f;
        if (input.is_gamepad_button_down(bud::input::GamepadButton::DPadUp))
            vx += 0.8f;
        if (input.is_gamepad_button_down(bud::input::GamepadButton::DPadDown))
            vx -= 0.4f;
    }

    // When steering without forward input, provide minimum stepping velocity so the RL gait activates and turns
    if (std::abs(yaw_rate) > 1.0e-3f && std::abs(vx) < 1.0e-3f && std::abs(vy) < 1.0e-3f)
        vx = 0.30f;

    // Clamp to the range the policy was trained on; commanding outside it degrades the gait.
    return bud::math::vec3(std::clamp(vx, -0.5f, 1.0f), std::clamp(vy, -0.3f, 0.3f),
                           std::clamp(yaw_rate, -0.5f, 0.5f));
}

void RobotAvatarController::toggle_camera_mode(bud::scene::Camera& camera) {
    if (m_view_mode == AvatarCameraView::ThirdPerson)
        set_camera_view(AvatarCameraView::FirstPerson, camera);
    else
        set_camera_view(AvatarCameraView::ThirdPerson, camera);
}

void RobotAvatarController::set_camera_view(AvatarCameraView view, bud::scene::Camera& camera) {
    m_view_mode = view;
    if (view == AvatarCameraView::ThirdPerson) {
        camera.set_mode(bud::scene::CameraMode::ThirdPerson);
        camera.orbit_distance = 2.4f;
        camera.orbit_pitch = -15.0f;
        if (m_robot && m_robot->is_simulation()) {
            const std::string& root_name = !m_robot->get_definition().root_link.empty()
                                               ? m_robot->get_definition().root_link
                                               : "pelvis";
            const glm::vec3 fwd = m_robot->get_link_rotation(root_name) * glm::vec3(1.0f, 0.0f, 0.0f);
            const float cur_sim_yaw = std::atan2(-fwd.z, fwd.x);
            camera.orbit_yaw = 90.0f + bud::math::degrees(cur_sim_yaw);
            m_prev_sim_yaw = cur_sim_yaw;
            m_has_prev_sim_yaw = true;
        } else {
            camera.orbit_yaw = 90.0f;
            m_has_prev_sim_yaw = false;
        }
        camera.target_position = get_torso_position();
        camera.update(0.0f);
        std::cout << "[RobotAvatarController] Switched to Third-Person View (Bound directly to G1 relative pose)" << std::endl;
    }
    else {
        camera.set_mode(bud::scene::CameraMode::FirstPerson);
        camera.position = get_head_camera_position();
        camera.pitch = 0.0f;
        camera.yaw = 0.0f;
        camera.rebuild_camera_vectors();
        std::cout << "[RobotAvatarController] Switched to First-Person View (G1 Visor relative pose)" << std::endl;
    }
}

void RobotAvatarController::recenter_camera(bud::scene::Camera& camera) {
    if (m_view_mode == AvatarCameraView::ThirdPerson) {
        if (m_robot && m_robot->is_simulation()) {
            const std::string& root_name = !m_robot->get_definition().root_link.empty()
                                               ? m_robot->get_definition().root_link
                                               : "pelvis";
            const glm::vec3 fwd = m_robot->get_link_rotation(root_name) * glm::vec3(1.0f, 0.0f, 0.0f);
            const float cur_sim_yaw = std::atan2(-fwd.z, fwd.x);
            camera.orbit_yaw = 90.0f + bud::math::degrees(cur_sim_yaw);
            m_prev_sim_yaw = cur_sim_yaw;
            m_has_prev_sim_yaw = true;
        } else {
            camera.orbit_yaw = 90.0f;
            m_has_prev_sim_yaw = false;
        }
        camera.orbit_pitch = -15.0f;
        camera.orbit_distance = 2.4f;
        camera.target_position = get_torso_position();
        camera.update(0.0f);
        std::cout << "[RobotAvatarController] Camera re-centered behind avatar" << std::endl;
    } else {
        camera.pitch = 0.0f;
        camera.yaw = 0.0f;
        camera.rebuild_camera_vectors();
        std::cout << "[RobotAvatarController] Camera re-centered forward" << std::endl;
    }
}

void RobotAvatarController::update(float dt, const bud::input::Input& input, bud::scene::Camera& camera) {
    if (!m_engine)
        return;

    // Simulation mode: driven purely by MuJoCo physical articulation
    if (m_robot && m_robot->is_simulation()) {
        const std::string& root_name = !m_robot->get_definition().root_link.empty()
                                           ? m_robot->get_definition().root_link
                                           : "pelvis";
        const auto real_pelvis_pos = m_robot->get_link_position(root_name);
        if (glm::length(real_pelvis_pos) > 1.0e-4f)
            m_current_pelvis_pos = real_pelvis_pos;

        // Process movement & steering input:
        // W / S: Forward / Backward along robot heading
        // A / D: Steer Left / Right (yaw rate)
        // Q / E: Lateral Strafe Left / Right
        float vx_cmd = 0.0f;
        float vy_cmd = 0.0f;
        float yaw_cmd = 0.0f;

        if (input.is_key_down(bud::input::Key::W))
            vx_cmd += 0.8f;
        if (input.is_key_down(bud::input::Key::S))
            vx_cmd -= 0.4f;
        if (input.is_key_down(bud::input::Key::A))
            yaw_cmd += 0.5f;
        if (input.is_key_down(bud::input::Key::D))
            yaw_cmd -= 0.5f;
        if (input.is_key_down(bud::input::Key::Q))
            vy_cmd += 0.25f;
        if (input.is_key_down(bud::input::Key::E))
            vy_cmd -= 0.25f;

        if (input.is_gamepad_connected()) {
            const auto deadzone = [](float value) {
                constexpr float kDeadzone = 0.15f;
                if (std::abs(value) < kDeadzone)
                    return 0.0f;
                return (value - std::copysign(kDeadzone, value)) / (1.0f - kDeadzone);
            };
            const float ly = -deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftY));
            const float lx = deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftX));

            // Left Stick: Y = forward/backward, X = steer left/right
            if (std::abs(ly) > 1.0e-3f)
                vx_cmd += (ly > 0.0f) ? (ly * 0.8f) : (ly * 0.4f);
            if (std::abs(lx) > 1.0e-3f)
                yaw_cmd += -lx * 0.5f;

            // Bumpers & D-Pad: Strafe left/right, D-Pad walk
            if (input.is_gamepad_button_down(bud::input::GamepadButton::LB) ||
                input.is_gamepad_button_down(bud::input::GamepadButton::DPadLeft))
                vy_cmd += 0.25f;
            if (input.is_gamepad_button_down(bud::input::GamepadButton::RB) ||
                input.is_gamepad_button_down(bud::input::GamepadButton::DPadRight))
                vy_cmd -= 0.25f;
            if (input.is_gamepad_button_down(bud::input::GamepadButton::DPadUp))
                vx_cmd += 0.8f;
            if (input.is_gamepad_button_down(bud::input::GamepadButton::DPadDown))
                vx_cmd -= 0.4f;
        }

        // When steering without forward input, provide minimum stepping velocity so the RL gait activates and turns
        if (std::abs(yaw_cmd) > 1.0e-3f && std::abs(vx_cmd) < 1.0e-3f && std::abs(vy_cmd) < 1.0e-3f)
            vx_cmd = 0.30f;

        if (m_policy && m_policy->ready()) {
            const bool has_user_input = (std::abs(vx_cmd) > 1.0e-3f || std::abs(vy_cmd) > 1.0e-3f || std::abs(yaw_cmd) > 1.0e-3f);
            if (has_user_input)
                m_has_policy_command_override = false;

            bud::math::vec3 command(0.0f);
            if (m_has_policy_command_override) {
                command = m_policy_command_override;
            } else if (has_user_input) {
                command = bud::math::vec3(std::clamp(vx_cmd, -0.5f, 1.0f),
                                          std::clamp(vy_cmd, -0.3f, 0.3f),
                                          std::clamp(yaw_cmd, -0.5f, 0.5f));
            }

            const bool is_translating = (std::abs(command.x) > 0.01f || std::abs(command.y) > 0.01f);
            m_is_moving = is_translating || (std::abs(command.z) > 0.01f);

            const glm::vec3 forward =
                m_robot->get_link_rotation("pelvis") * glm::vec3(1.0f, 0.0f, 0.0f);
            const float current_yaw = std::atan2(-forward.z, forward.x);
            if (!m_policy_yaw_initialized) {
                m_policy_target_yaw = current_yaw;
                m_policy_yaw_initialized = true;
            }

            if (m_policy_heading_hold) {
                if (std::abs(yaw_cmd) > 1.0e-3f) {
                    // Actively steering: target tracks where user steers
                    m_policy_target_yaw = current_yaw;
                } else if (is_translating) {
                    // Actively walking straight: hold current target heading
                    float error = m_policy_target_yaw - current_yaw;
                    while (error > 3.14159265f)
                        error -= 6.28318531f;
                    while (error < -3.14159265f)
                        error += 6.28318531f;
                    command.z = std::clamp(1.5f * error, -0.2f, 0.2f);
                } else {
                    // Standing idle: reset target, zero yaw velocity
                    m_policy_target_yaw = current_yaw;
                    command.z = 0.0f;
                }
            }
            m_policy->set_command(command);
            m_policy->update(dt);
        }
        else {
            bud::math::vec3 move_dir(0.0f);
            if (input.is_key_down(bud::input::Key::W))
                move_dir += camera.front;
            if (input.is_key_down(bud::input::Key::S))
                move_dir -= camera.front;
            if (input.is_key_down(bud::input::Key::A))
                move_dir -= camera.right;
            if (input.is_key_down(bud::input::Key::D))
                move_dir += camera.right;

            if (input.is_gamepad_connected()) {
                const auto deadzone = [](float value) {
                    constexpr float kDeadzone = 0.15f;
                    if (std::abs(value) < kDeadzone)
                        return 0.0f;
                    return (value - std::copysign(kDeadzone, value)) / (1.0f - kDeadzone);
                };
                float lx = deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftX));
                float ly = -deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftY));
                move_dir += camera.right * lx;
                move_dir += camera.front * ly;
            }

            move_dir.y = 0.0f;
            const float move_len = glm::length(move_dir);

            if (move_len > 0.001f) {
                move_dir /= move_len;
                m_is_moving = true;
                m_current_yaw = std::atan2(-move_dir.x, -move_dir.z);
                apply_walking_gait(dt, move_len);

                std::vector<physics::JointCommand> commands;
                commands.reserve(m_target_joint_angles.size());
                for (const auto& [name, target_q] : m_target_joint_angles) {
                    physics::JointCommand jc;
                    jc.joint_name = name;
                    jc.q = target_q;
                    jc.dq = 0.0f;
                    get_default_g1_gains(name, jc.kp, jc.kd);
                    jc.tau_ff = 0.0f;
                    commands.push_back(std::move(jc));
                }
                m_robot->set_joint_commands(commands);
            }
            else {
                m_is_moving = false;
                reset_to_idle_stance();

            const bud::math::quaternion pelvis_rot = m_robot->get_link_rotation(root_name);
            float pitch_rad = 0.0f;
            float roll_rad = 0.0f;
            float tilt_deg = 0.0f;
            compute_body_orientation(pelvis_rot, pitch_rad, roll_rad, tilt_deg);
            const float pitch_vel = (dt > 1.0e-5f) ? (pitch_rad - m_prev_pitch) / dt : 0.0f;
            const float roll_vel = (dt > 1.0e-5f) ? (roll_rad - m_prev_roll) / dt : 0.0f;
            m_prev_pitch = pitch_rad;
            m_prev_roll = roll_rad;

                LowCmd cmd = make_g1_standing_cmd();
                apply_standing_balance(cmd, pitch_rad, pitch_vel, roll_rad, roll_vel);
                m_robot->set_low_cmd(cmd);
            }
        }

        // Sync joint angles from physical simulation to forward kinematics state
        for (const auto& joint : m_robot->get_definition().joints) {
            if (!joint.name.empty())
                m_current_joint_angles[joint.name] = m_robot->get_joint_angle(joint.name);
        }

        // Calculate full-body forward kinematics for 100% rigid visual assembly
        update_forward_kinematics();

        const bud::math::quaternion pelvis_rot = m_robot->get_link_rotation(root_name);
        bud::math::quaternion root_rot;
        if (m_robot->is_simulation())
            root_rot = pelvis_rot * m_urdf_to_world_rot;
        else {
            bud::math::quaternion yaw_rot = glm::angleAxis(m_current_yaw, bud::math::vec3(0.0f, 1.0f, 0.0f));
            root_rot = yaw_rot * pelvis_rot * m_urdf_to_world_rot;
        }

        bud::math::mat4 root_world_mat = glm::translate(bud::math::mat4(1.0f), m_current_pelvis_pos)
                                       * glm::mat4_cast(root_rot);

        auto& scene = m_engine->get_scene();
        if (m_visual_bridge) {
            std::unordered_map<std::string, glm::mat4> world_link_transforms;
            world_link_transforms.reserve(m_current_link_xforms.size());
            for (const auto& [name, local_mat] : m_current_link_xforms)
                world_link_transforms[name] = root_world_mat * local_mat;
            m_visual_bridge->sync_transforms(world_link_transforms, scene);
        }

        if (m_view_mode == AvatarCameraView::ThirdPerson) {
            camera.target_position = get_torso_position();
            const glm::vec3 fwd_vec = m_robot->get_link_rotation(root_name) * glm::vec3(1.0f, 0.0f, 0.0f);
            const float cur_sim_yaw = std::atan2(-fwd_vec.z, fwd_vec.x);
            if (m_has_prev_sim_yaw) {
                float delta = cur_sim_yaw - m_prev_sim_yaw;
                while (delta > 3.14159265f)
                    delta -= 6.28318531f;
                while (delta < -3.14159265f)
                    delta += 6.28318531f;
                camera.orbit_yaw += bud::math::degrees(delta);
            }
            m_prev_sim_yaw = cur_sim_yaw;
            m_has_prev_sim_yaw = true;
            camera.update(dt);
        } else {
            camera.position = get_head_camera_position();
        }

        if (auto* controller = m_engine->get_character_controller()) {
            const bud::math::vec3 cur_pos = m_robot->get_link_position(root_name);
            const bud::math::vec3 prev_pos = controller->get_position();
            bud::math::vec3 vel(0.0f);
            if (dt > 1e-5f)
                vel = (cur_pos - prev_pos) / dt;
            controller->teleport(cur_pos);
            controller->set_velocity(vel);
        }
        return;
    }

    auto* controller = m_engine->get_character_controller();
    if (controller) {
        bud::math::vec3 move_dir(0.0f);
        if (input.is_key_down(bud::input::Key::W))
            move_dir += camera.front;
        if (input.is_key_down(bud::input::Key::S))
            move_dir -= camera.front;
        if (input.is_key_down(bud::input::Key::A))
            move_dir -= camera.right;
        if (input.is_key_down(bud::input::Key::D))
            move_dir += camera.right;

        if (input.is_gamepad_connected()) {
            const auto deadzone = [](float value) {
                constexpr float kDeadzone = 0.15f;
                if (std::abs(value) < kDeadzone)
                    return 0.0f;
                return (value - std::copysign(kDeadzone, value)) / (1.0f - kDeadzone);
            };
            float lx = deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftX));
            float ly = -deadzone(input.get_gamepad_axis(bud::input::GamepadAxis::LeftY));
            move_dir += camera.right * lx;
            move_dir += camera.front * ly;
        }

        move_dir.y = 0.0f;
        float move_len = glm::length(move_dir);
        if (move_len > 0.001f)
            move_dir = glm::normalize(move_dir);

        controller->set_velocity(move_dir * k_walk_speed);
        controller->update(dt);

        // Determine pelvis world height directly from physics controller and ground raycast
        bud::math::vec3 char_pos = controller->get_position();
        float capsule_ground_offset = controller->get_capsule_height() * 0.5f + controller->get_capsule_radius();
        float capsule_base_y = char_pos.y - capsule_ground_offset;

        float ground_y = capsule_base_y;
        bool has_ground = get_ground_height(char_pos, ground_y);

        const bool grounded = controller->is_grounded();
        // Ground-relative pelvis height with a small clearance and a rate limit.
        // The previous version swept the kinematic pelvis to the exact raycast surface
        // every frame ("hard snap"). That is not physical: it pushes the dynamic legs
        // and feet centimetres into the floor mesh, and such deep convex-vs-mesh
        // penetration makes Jolt's EPA run away in release builds (its internal buffers
        // are only guarded by assertions, which are compiled out there). Keeping the
        // soles slightly above the surface, and never moving the pelvis faster than the
        // rate limit, bounds the penetration to millimetres while still tracking steps.
        constexpr float kGroundClearance = 0.03f;   // soles rest ~3 cm above the mesh
        constexpr float kMaxVerticalSpeed = 0.3f;   // m/s, no teleports
        constexpr float kMaxHorizontalSpeed = 2.5f; // m/s, damps stair-step jumps of the controller
        float base_y = (grounded && has_ground) ? (ground_y + kGroundClearance) : capsule_base_y;
        float target_pelvis_y = base_y - k_mesh_foot_sole_offset_y;

        m_is_moving = (move_len > 0.01f) && grounded;
        if (m_is_moving) {
            float target_yaw = std::atan2(-move_dir.x, -move_dir.z);
            m_current_yaw = target_yaw;
            apply_walking_gait(dt, k_walk_speed);

            // Natural pelvis vertical bounce during walking
            float bob_offset = 0.022f * (std::cos(2.0f * m_gait_phase) - 1.0f) * 0.5f;
            target_pelvis_y += bob_offset;
        }
        else {
            reset_to_idle_stance();
        }

        // Drive the kinematic pelvis toward that height without teleporting: start from
        // the pelvis body's current physics position and move at most kMaxVerticalSpeed.
        float current_pelvis_y = target_pelvis_y;
        if (m_robot && m_robot->get_ragdoll()) {
            auto* system = m_engine->get_physics_scene()->get_jolt_system();
            if (system)
                current_pelvis_y = static_cast<float>(
                    system->GetBodyInterface().GetPosition(m_robot->get_ragdoll()->GetBodyID(0)).GetY());
        }
        const float max_vertical_step = kMaxVerticalSpeed * dt;
        const float pelvis_y = current_pelvis_y +
            std::clamp(target_pelvis_y - current_pelvis_y, -max_vertical_step, max_vertical_step);

        // Horizontal follow of the kinematic pelvis, rate limited as well: the character
        // controller can jump up/down stairs by up to 0.4-0.5 m in a single frame, and
        // following that instantly sweeps the dynamic legs sideways into the step mesh.
        const float max_horizontal_step = kMaxHorizontalSpeed * dt;
        const float prev_x = m_current_pelvis_pos.x;
        const float prev_z = m_current_pelvis_pos.z;
        const float clamped_x = prev_x + std::clamp(char_pos.x - prev_x, -max_horizontal_step, max_horizontal_step);
        const float clamped_z = prev_z + std::clamp(char_pos.z - prev_z, -max_horizontal_step, max_horizontal_step);

        m_current_pelvis_pos = bud::math::vec3(clamped_x, pelvis_y, clamped_z);

        static bool s_was_grounded = false;
        if (grounded && !s_was_grounded)
            std::cout << "[G1 Avatar] Touchdown on ground: sole_y = " << (pelvis_y + k_mesh_foot_sole_offset_y) << " m." << std::endl;
        s_was_grounded = grounded;

        // Kinematic anchoring of robot root body to pelvis world position
        bud::math::quaternion yaw_rot = glm::angleAxis(m_current_yaw, bud::math::vec3(0.0f, 1.0f, 0.0f));
        bud::math::quaternion world_rot = yaw_rot * m_urdf_to_world_rot;

        if (m_robot && m_robot->get_ragdoll()) {
            auto* system = m_engine->get_physics_scene()->get_jolt_system();
            if (system) {
                JPH::BodyID pelvis_bid = m_robot->get_ragdoll()->GetBodyID(0);
                JPH::RVec3 j_pos(m_current_pelvis_pos.x, m_current_pelvis_pos.y, m_current_pelvis_pos.z);
                JPH::Quat j_rot(world_rot.x, world_rot.y, world_rot.z, world_rot.w);
                system->GetBodyInterface().MoveKinematic(pelvis_bid, j_pos, j_rot, std::max(dt, 1.0f / 120.0f));
            }
        }
    }

    // Smoothly interpolate joint angles toward targets
    for (const auto& [name, target_val] : m_target_joint_angles) {
        float& cur = m_current_joint_angles[name];
        cur += (target_val - cur) * std::min(1.0f, dt * 15.0f);
    }

    // Drive Jolt physics motorized joints with smoothed angles
    if (m_robot) {
        for (const auto& [name, cur_val] : m_current_joint_angles) {
            m_robot->set_joint_target_angle(name, cur_val);
        }
    }

    // Calculate full-body forward kinematics for rigid link transforms
    update_forward_kinematics();

    bud::math::quaternion yaw_rot = glm::angleAxis(m_current_yaw, bud::math::vec3(0.0f, 1.0f, 0.0f));
    bud::math::mat4 root_world_mat = glm::translate(bud::math::mat4(1.0f), m_current_pelvis_pos) * glm::mat4_cast(yaw_rot * m_urdf_to_world_rot);

    auto& scene = m_engine->get_scene();

    // Articulated Multi-Link Rigid Hierarchy: 100% rigid, zero joint slop/separation
    if (m_visual_bridge) {
        std::unordered_map<std::string, glm::mat4> world_link_transforms;
        world_link_transforms.reserve(m_current_link_xforms.size());
        for (const auto& [name, local_mat] : m_current_link_xforms) {
            world_link_transforms[name] = root_world_mat * local_mat;
        }
        m_visual_bridge->sync_transforms(world_link_transforms, scene);
    }

    // Keep Jolt physics rigid bodies strictly aligned with the rigid hierarchy
    if (m_robot && m_robot->get_ragdoll()) {
        auto* system = m_engine->get_physics_scene()->get_jolt_system();
        if (system) {
            for (const auto& [name, local_mat] : m_current_link_xforms) {
                int part_idx = m_robot->get_part_index(name);
                if (part_idx >= 0 && static_cast<size_t>(part_idx) < m_robot->get_ragdoll()->GetBodyCount()) {
                    JPH::BodyID bid = m_robot->get_ragdoll()->GetBodyID(part_idx);
                    glm::mat4 wm = root_world_mat * local_mat;
                    glm::vec3 pos = glm::vec3(wm[3]);
                    glm::quat rot = glm::quat_cast(wm);
                    system->GetBodyInterface().SetPositionAndRotation(bid,
                        JPH::RVec3(pos.x, pos.y, pos.z),
                        JPH::Quat(rot.x, rot.y, rot.z, rot.w),
                        JPH::EActivation::Activate);
                }
            }
        }
    }

    // Update Camera based on view mode (directly bound to G1 relative geometry)
    if (m_view_mode == AvatarCameraView::ThirdPerson) {
        camera.target_position = get_torso_position();
        camera.update(dt);
    }
    else {
        camera.position = get_head_camera_position();
    }
}

void RobotAvatarController::apply_walking_gait(float dt, float speed) {
    if (!m_robot)
        return;

    float speed_factor = std::clamp(speed / 1.6f, 0.5f, 1.5f);
    m_gait_phase += dt * (k_gait_frequency * speed_factor);
    if (m_gait_phase > 6.2831853f)
        m_gait_phase -= 6.2831853f;

    float sin_p = std::sin(m_gait_phase);
    float cos_p = std::cos(m_gait_phase);

    // Left leg gait
    m_target_joint_angles["left_hip_pitch_joint"] = k_idle_hip_pitch - sin_p * k_hip_amplitude;
    float l_knee = (sin_p > 0.0f) ? (sin_p * k_knee_amplitude + k_idle_knee_angle) : k_idle_knee_angle;
    m_target_joint_angles["left_knee_joint"] = l_knee;
    m_target_joint_angles["left_ankle_pitch_joint"] = (sin_p > 0.0f) ? (k_idle_ankle_pitch - sin_p * 0.20f) : k_idle_ankle_pitch;

    // Right leg gait (anti-phase)
    m_target_joint_angles["right_hip_pitch_joint"] = k_idle_hip_pitch + sin_p * k_hip_amplitude;
    float r_knee = (sin_p < 0.0f) ? (-sin_p * k_knee_amplitude + k_idle_knee_angle) : k_idle_knee_angle;
    m_target_joint_angles["right_knee_joint"] = r_knee;
    m_target_joint_angles["right_ankle_pitch_joint"] = (sin_p < 0.0f) ? (k_idle_ankle_pitch + sin_p * 0.20f) : k_idle_ankle_pitch;

    // Torso gentle counter-rotation
    m_target_joint_angles["waist_yaw_joint"] = -sin_p * 0.05f;

    // Arm swing
    m_target_joint_angles["left_shoulder_pitch_joint"] = 0.2f + sin_p * k_arm_amplitude;
    m_target_joint_angles["left_elbow_joint"] = k_idle_elbow_angle + std::max(0.0f, sin_p) * 0.2f;

    m_target_joint_angles["right_shoulder_pitch_joint"] = 0.2f - sin_p * k_arm_amplitude;
    m_target_joint_angles["right_elbow_joint"] = k_idle_elbow_angle + std::max(0.0f, -sin_p) * 0.2f;
}

void RobotAvatarController::reset_to_idle_stance() {
    m_gait_phase = 0.0f;

    // Neutral upright standing pose
    m_target_joint_angles["left_hip_pitch_joint"] = k_idle_hip_pitch;
    m_target_joint_angles["left_hip_roll_joint"] = 0.0f;
    m_target_joint_angles["left_hip_yaw_joint"] = 0.0f;
    m_target_joint_angles["left_knee_joint"] = k_idle_knee_angle;
    m_target_joint_angles["left_ankle_pitch_joint"] = k_idle_ankle_pitch;
    m_target_joint_angles["left_ankle_roll_joint"] = 0.0f;

    m_target_joint_angles["right_hip_pitch_joint"] = k_idle_hip_pitch;
    m_target_joint_angles["right_hip_roll_joint"] = 0.0f;
    m_target_joint_angles["right_hip_yaw_joint"] = 0.0f;
    m_target_joint_angles["right_knee_joint"] = k_idle_knee_angle;
    m_target_joint_angles["right_ankle_pitch_joint"] = k_idle_ankle_pitch;
    m_target_joint_angles["right_ankle_roll_joint"] = 0.0f;

    m_target_joint_angles["waist_yaw_joint"] = 0.0f;
    m_target_joint_angles["waist_roll_joint"] = 0.0f;
    m_target_joint_angles["waist_pitch_joint"] = 0.0f;

    m_target_joint_angles["left_shoulder_pitch_joint"] = 0.2f;
    m_target_joint_angles["left_shoulder_roll_joint"] = 0.2f;
    m_target_joint_angles["left_shoulder_yaw_joint"] = 0.0f;
    m_target_joint_angles["left_elbow_joint"] = k_idle_elbow_angle;

    m_target_joint_angles["right_shoulder_pitch_joint"] = 0.2f;
    m_target_joint_angles["right_shoulder_roll_joint"] = -0.2f;
    m_target_joint_angles["right_shoulder_yaw_joint"] = 0.0f;
    m_target_joint_angles["right_elbow_joint"] = k_idle_elbow_angle;
}

void RobotAvatarController::update_forward_kinematics() {
    if (!m_robot)
        return;

    const auto& def = m_robot->get_definition();
    if (def.root_link.empty())
        return;

    m_current_link_xforms.clear();
    m_current_link_xforms[def.root_link] = glm::mat4(1.0f);

    std::queue<std::string> q;
    q.push(def.root_link);

    std::unordered_set<std::string> visited;
    visited.insert(def.root_link);

    while (!q.empty()) {
        std::string parent_name = q.front();
        q.pop();

        glm::mat4 parent_mat = m_current_link_xforms[parent_name];
        auto child_joints = def.get_child_joints(parent_name);

        for (const auto* joint : child_joints) {
            if (!joint || joint->child_link.empty())
                continue;
            if (visited.find(joint->child_link) != visited.end())
                continue;

            glm::vec3 j_pos(joint->origin_xyz[0], joint->origin_xyz[1], joint->origin_xyz[2]);
            float roll = joint->origin_rpy[0];
            float pitch = joint->origin_rpy[1];
            float yaw = joint->origin_rpy[2];

            glm::quat j_rot = glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f))
                            * glm::angleAxis(pitch, glm::vec3(0.0f, 1.0f, 0.0f))
                            * glm::angleAxis(roll, glm::vec3(1.0f, 0.0f, 0.0f));

            float angle = 0.0f;
            auto it = m_current_joint_angles.find(joint->name);
            if (it != m_current_joint_angles.end())
                angle = it->second;

            glm::vec3 axis(joint->axis[0], joint->axis[1], joint->axis[2]);
            float axis_len = glm::length(axis);
            glm::mat4 rot = glm::mat4(1.0f);
            if (axis_len > 1e-4f && std::abs(angle) > 1e-6f) {
                rot = glm::rotate(glm::mat4(1.0f), angle, axis / axis_len);
            }

            glm::mat4 joint_local = glm::translate(glm::mat4(1.0f), j_pos) * glm::mat4_cast(j_rot) * rot;
            glm::mat4 child_mat = parent_mat * joint_local;

            m_current_link_xforms[joint->child_link] = child_mat;
            visited.insert(joint->child_link);
            q.push(joint->child_link);
        }
    }
}

bud::math::vec3 RobotAvatarController::get_pelvis_position() const {
    return m_current_pelvis_pos;
}

bud::math::vec3 RobotAvatarController::get_torso_position() const {
    if (m_robot && m_robot->is_simulation()) {
        const auto torso_pos = m_robot->get_link_position("torso_link");
        if (glm::length(torso_pos) > 1.0e-4f)
            return torso_pos;
    }
    return m_current_pelvis_pos + bud::math::vec3(0.0f, k_torso_relative_y, 0.0f);
}

bud::math::vec3 RobotAvatarController::get_head_camera_position() const {
    if (m_robot && m_robot->is_simulation()) {
        const auto head_pos = m_robot->get_link_position("head_link");
        if (glm::length(head_pos) > 1.0e-4f) {
            const auto head_rot = m_robot->get_link_rotation("head_link");
            const auto fwd = head_rot * bud::math::vec3(1.0f, 0.0f, 0.0f);
            return head_pos + fwd * k_head_fwd_offset;
        }
    }
    bud::math::quaternion yaw_rot = glm::angleAxis(m_current_yaw, bud::math::vec3(0.0f, 1.0f, 0.0f));
    bud::math::vec3 fwd = yaw_rot * bud::math::vec3(0.0f, 0.0f, -1.0f);
    return m_current_pelvis_pos + bud::math::vec3(0.0f, k_head_relative_y, 0.0f) + fwd * k_head_fwd_offset;
}

void RobotAvatarController::get_debug_collision_vertices(std::vector<bud::graphics::PhysicsDebugVertex>& out_verts) const {
    if (!m_robot)
        return;

    const auto& def = m_robot->get_definition();
    bud::math::quaternion yaw_rot = glm::angleAxis(m_current_yaw, bud::math::vec3(0.0f, 1.0f, 0.0f));
    bud::math::mat4 root_world_mat = glm::translate(bud::math::mat4(1.0f), m_current_pelvis_pos) * glm::mat4_cast(yaw_rot * m_urdf_to_world_rot);

    // Warm amber / orange color for robot collision wireframe
    const bud::math::vec3 col_color(1.0f, 0.70f, 0.15f);

    for (const auto& link : def.links) {
        auto it = m_current_link_xforms.find(link.name);
        if (it == m_current_link_xforms.end())
            continue;

        glm::mat4 link_world = root_world_mat * it->second;

        for (const auto& col : link.collisions) {
            glm::vec3 c_pos(col.origin_xyz[0], col.origin_xyz[1], col.origin_xyz[2]);
            glm::quat c_rot = glm::angleAxis(col.origin_rpy[2], glm::vec3(0.0f, 0.0f, 1.0f))
                            * glm::angleAxis(col.origin_rpy[1], glm::vec3(0.0f, 1.0f, 0.0f))
                            * glm::angleAxis(col.origin_rpy[0], glm::vec3(1.0f, 0.0f, 0.0f));
            glm::mat4 col_local = glm::translate(glm::mat4(1.0f), c_pos) * glm::mat4_cast(c_rot);
            glm::mat4 col_world = link_world * col_local;

            if (col.geometry.type == GeometryType::Mesh && !col.convex_hull.points.empty()) {
                const auto& pts = col.convex_hull.points;
                const auto& idxs = col.convex_hull.indices;
                if (!idxs.empty()) {
                    for (size_t i = 0; i + 2 < idxs.size(); i += 3) {
                        uint32_t a = idxs[i];
                        uint32_t b = idxs[i + 1];
                        uint32_t c = idxs[i + 2];
                        if (a * 3 + 2 < pts.size() && b * 3 + 2 < pts.size() && c * 3 + 2 < pts.size()) {
                            glm::vec3 p0 = glm::vec3(col_world * glm::vec4(pts[a * 3], pts[a * 3 + 1], pts[a * 3 + 2], 1.0f));
                            glm::vec3 p1 = glm::vec3(col_world * glm::vec4(pts[b * 3], pts[b * 3 + 1], pts[b * 3 + 2], 1.0f));
                            glm::vec3 p2 = glm::vec3(col_world * glm::vec4(pts[c * 3], pts[c * 3 + 1], pts[c * 3 + 2], 1.0f));
                            out_verts.push_back({{p0.x, p0.y, p0.z}, {col_color.x, col_color.y, col_color.z}});
                            out_verts.push_back({{p1.x, p1.y, p1.z}, {col_color.x, col_color.y, col_color.z}});
                            out_verts.push_back({{p1.x, p1.y, p1.z}, {col_color.x, col_color.y, col_color.z}});
                            out_verts.push_back({{p2.x, p2.y, p2.z}, {col_color.x, col_color.y, col_color.z}});
                            out_verts.push_back({{p2.x, p2.y, p2.z}, {col_color.x, col_color.y, col_color.z}});
                            out_verts.push_back({{p0.x, p0.y, p0.z}, {col_color.x, col_color.y, col_color.z}});
                        }
                    }
                }
            } else if (col.geometry.type == GeometryType::Box) {
                float hx = col.geometry.box_size[0] * 0.5f;
                float hy = col.geometry.box_size[1] * 0.5f;
                float hz = col.geometry.box_size[2] * 0.5f;
                glm::vec3 corners[8] = {
                    glm::vec3(col_world * glm::vec4(-hx, -hy, -hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4( hx, -hy, -hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4( hx,  hy, -hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4(-hx,  hy, -hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4(-hx, -hy,  hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4( hx, -hy,  hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4( hx,  hy,  hz, 1.0f)),
                    glm::vec3(col_world * glm::vec4(-hx,  hy,  hz, 1.0f)),
                };
                static const int box_edges[24] = {
                    0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4,
                    0, 4, 1, 5, 2, 6, 3, 7
                };
                for (int e = 0; e < 24; ++e) {
                    const auto& p = corners[box_edges[e]];
                    out_verts.push_back({{p.x, p.y, p.z}, {col_color.x, col_color.y, col_color.z}});
                }
            } else if (col.geometry.type == GeometryType::Cylinder || col.geometry.type == GeometryType::Capsule) {
                float r = (col.geometry.type == GeometryType::Cylinder) ? col.geometry.cylinder_radius : col.geometry.capsule_radius;
                float len = (col.geometry.type == GeometryType::Cylinder) ? col.geometry.cylinder_length : col.geometry.capsule_length;
                float hz = len * 0.5f;
                constexpr int k_seg = 12;
                constexpr float k_two_pi = 6.2831853f;
                for (int i = 0; i < k_seg; ++i) {
                    float a0 = k_two_pi * float(i) / float(k_seg);
                    float a1 = k_two_pi * float(i + 1) / float(k_seg);
                    float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r;
                    float c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;
                    glm::vec3 t0 = glm::vec3(col_world * glm::vec4(c0, s0, hz, 1.0f));
                    glm::vec3 t1 = glm::vec3(col_world * glm::vec4(c1, s1, hz, 1.0f));
                    out_verts.push_back({{t0.x, t0.y, t0.z}, {col_color.x, col_color.y, col_color.z}});
                    out_verts.push_back({{t1.x, t1.y, t1.z}, {col_color.x, col_color.y, col_color.z}});

                    glm::vec3 b0 = glm::vec3(col_world * glm::vec4(c0, s0, -hz, 1.0f));
                    glm::vec3 b1 = glm::vec3(col_world * glm::vec4(c1, s1, -hz, 1.0f));
                    out_verts.push_back({{b0.x, b0.y, b0.z}, {col_color.x, col_color.y, col_color.z}});
                    out_verts.push_back({{b1.x, b1.y, b1.z}, {col_color.x, col_color.y, col_color.z}});
                }
                for (int i = 0; i < 4; ++i) {
                    float a = k_two_pi * float(i) / 4.0f;
                    float c = std::cos(a) * r, s = std::sin(a) * r;
                    glm::vec3 p0 = glm::vec3(col_world * glm::vec4(c, s, -hz, 1.0f));
                    glm::vec3 p1 = glm::vec3(col_world * glm::vec4(c, s,  hz, 1.0f));
                    out_verts.push_back({{p0.x, p0.y, p0.z}, {col_color.x, col_color.y, col_color.z}});
                    out_verts.push_back({{p1.x, p1.y, p1.z}, {col_color.x, col_color.y, col_color.z}});
                }
            } else if (col.geometry.type == GeometryType::Sphere) {
                float r = col.geometry.sphere_radius;
                constexpr int k_seg = 12;
                constexpr float k_two_pi = 6.2831853f;
                for (int i = 0; i < k_seg; ++i) {
                    float a0 = k_two_pi * float(i) / float(k_seg);
                    float a1 = k_two_pi * float(i + 1) / float(k_seg);
                    float c0 = std::cos(a0) * r, s0 = std::sin(a0) * r;
                    float c1 = std::cos(a1) * r, s1 = std::sin(a1) * r;
                    glm::vec3 xy0 = glm::vec3(col_world * glm::vec4(c0, s0, 0.0f, 1.0f));
                    glm::vec3 xy1 = glm::vec3(col_world * glm::vec4(c1, s1, 0.0f, 1.0f));
                    out_verts.push_back({{xy0.x, xy0.y, xy0.z}, {col_color.x, col_color.y, col_color.z}});
                    out_verts.push_back({{xy1.x, xy1.y, xy1.z}, {col_color.x, col_color.y, col_color.z}});

                    glm::vec3 yz0 = glm::vec3(col_world * glm::vec4(0.0f, c0, s0, 1.0f));
                    glm::vec3 yz1 = glm::vec3(col_world * glm::vec4(0.0f, c1, s1, 1.0f));
                    out_verts.push_back({{yz0.x, yz0.y, yz0.z}, {col_color.x, col_color.y, col_color.z}});
                    out_verts.push_back({{yz1.x, yz1.y, yz1.z}, {col_color.x, col_color.y, col_color.z}});

                    glm::vec3 zx0 = glm::vec3(col_world * glm::vec4(s0, 0.0f, c0, 1.0f));
                    glm::vec3 zx1 = glm::vec3(col_world * glm::vec4(s1, 0.0f, c1, 1.0f));
                    out_verts.push_back({{zx0.x, zx0.y, zx0.z}, {col_color.x, col_color.y, col_color.z}});
                    out_verts.push_back({{zx1.x, zx1.y, zx1.z}, {col_color.x, col_color.y, col_color.z}});
                }
            }
        }
    }
}

} // namespace bud::robots
