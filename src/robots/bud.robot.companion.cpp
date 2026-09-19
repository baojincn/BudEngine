#include "src/robots/bud.robot.companion.hpp"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <glm/gtc/quaternion.hpp>

#include "src/runtime/bud.engine.hpp"
#include "src/physics/bud.physics.scene.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"
#include "src/core/bud.logger.hpp"

namespace bud::robots {

namespace {

constexpr float k_follow_distance = 1.20f;   // Safe follow distance behind G1 (1.2m)
constexpr float k_max_forward_speed = 0.35f; // Max duck walking speed m/s
constexpr float k_max_turn_rate = 0.60f;     // Max duck turning rate rad/s (~34 deg/s, safe for biped stability)
constexpr float k_pi = 3.1415926535f;

float wrap_angle(float angle) {
    while (angle > k_pi)
        angle -= 2.0f * k_pi;
    while (angle < -k_pi)
        angle += 2.0f * k_pi;
    return angle;
}

} // namespace

MicroduckCompanionController::MicroduckCompanionController() = default;
MicroduckCompanionController::~MicroduckCompanionController() = default;

bool MicroduckCompanionController::init(
    bud::engine::BudEngine* engine,
    const bud::math::vec3& spawn_offset,
    const std::string& robot_asset_path,
    const std::string& package_root
) {
    if (!engine)
        return false;

    engine_ptr = engine;
    auto* physics_scene = engine->get_physics_scene();
    if (!physics_scene) {
        bud::eprint("[MicroduckCompanion] Physics scene is null");
        return false;
    }

    RobotSpawnParams params{};
    params.position = spawn_offset;
    params.rotation = bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    params.activate = true;
    params.enable_motors = true;
    params.default_motor_stiffness = 0.55f;
    params.default_motor_damping = 0.053f;
    params.default_motor_max_torque = 0.96f;
    params.asset_path = robot_asset_path;
    params.initial_joint_angles = get_microduck_standing_joint_angles();

    bud::print("[MicroduckCompanion] Spawning Microduck from: {}...", robot_asset_path);
    robot = RobotLoader::spawn_robot_from_file(*physics_scene, robot_asset_path, params);
    if (!robot) {
        bud::eprint("[MicroduckCompanion] Failed to spawn Microduck from: {}", robot_asset_path);
        return false;
    }

    visual_bridge = std::make_unique<RobotVisualBridge>();
    if (!visual_bridge->init(engine, engine->get_scene(), robot->get_definition(), package_root)) {
        bud::eprint("[MicroduckCompanion] Failed to init visual bridge for Microduck");
        return false;
    }
    visual_bridge->set_simulation_mode(true);

    // Show the duck upright at its spawn slot immediately. The MuJoCo world (and therefore the
    // articulation state sync_transforms reads) is not available until prepare_simulation(), so
    // without this it renders at the origin in the mesh's native Z-up orientation until then.
    visual_bridge->place_rest_pose(robot->get_definition(), params.initial_joint_angles, spawn_offset,
                                   engine->get_scene());

    current_position = spawn_offset;
    initialized = true;
    bud::print("[MicroduckCompanion] Spawned at ({:.2f}, {:.2f}, {:.2f}) — call start_policy() after prepare_simulation()",
               spawn_offset.x, spawn_offset.y, spawn_offset.z);
    return true;
}

bool MicroduckCompanionController::start_policy(const std::string& onnx_policy_path) {
    if (!initialized || !robot || !engine_ptr) {
        bud::eprint("[MicroduckCompanion] start_policy() called before init()");
        return false;
    }
    auto* physics_scene = engine_ptr->get_physics_scene();
    if (!physics_scene)
        return false;

    auto new_controller = std::make_unique<rl::MicroduckPolicyController>();
    std::string policy_error;
    if (!new_controller->initialize(physics_scene->get_world(), robot->get_articulation_handle(),
                                    robot->get_definition(), onnx_policy_path, policy_error)) {
        bud::eprint("[MicroduckCompanion] Failed to initialize policy controller: {}", policy_error);
        return false;
    }

    if (auto* scheduler = engine_ptr->get_task_scheduler()) {
        scheduler->submit_main_thread_task([this, ctrl = std::move(new_controller), onnx_policy_path]() mutable {
            policy_controller = std::move(ctrl);
            bud::print("[MicroduckCompanion] Policy activated: {}", onnx_policy_path);
        });
    }
    else {
        policy_controller = std::move(new_controller);
        bud::print("[MicroduckCompanion] Policy activated: {}", onnx_policy_path);
    }
    return true;
}

void MicroduckCompanionController::update_follower(
    float dt,
    const bud::math::vec3& g1_pelvis_pos,
    const bud::math::vec3& g1_forward
) {
    if (!initialized || !robot || !engine_ptr)
        return;

    current_position = robot->get_link_position("trunk_base");

    bud::math::vec3 horizontal_fwd = glm::normalize(bud::math::vec3(g1_forward.x, 0.0f, g1_forward.z));
    if (glm::length(horizontal_fwd) < 1e-4f)
        horizontal_fwd = bud::math::vec3(1.0f, 0.0f, 0.0f);

    const bud::math::vec3 follow_slot = g1_pelvis_pos - horizontal_fwd * k_follow_distance;

    const float delta_x = follow_slot.x - current_position.x;
    const float delta_z = follow_slot.z - current_position.z;
    const float dist = std::sqrt(delta_x * delta_x + delta_z * delta_z);

    constexpr float k_arrive_radius = 0.12f;       // 12cm arrival zone around slot
    constexpr float k_heading_blend_dist = 0.35f;  // within 35cm, smoothly blend to G1 heading
    constexpr float k_min_walk_speed = 0.16f;      // minimum speed for reliable leg stepping
    constexpr float k_max_accel = 0.8f;            // m/s^2 forward acceleration
    constexpr float k_max_decel = 1.2f;            // m/s^2 forward deceleration (smooth braking)
    constexpr float k_max_yaw_accel = 2.5f;        // rad/s^2 yaw acceleration

    const float g1_heading = std::atan2(-horizontal_fwd.z, horizontal_fwd.x);

    float desired_heading = g1_heading;
    if (dist > 1.0e-4f) {
        const float to_slot_heading = std::atan2(-delta_z, delta_x);
        if (dist >= k_heading_blend_dist)
            desired_heading = to_slot_heading;
        else {
            const float blend = std::clamp((dist - k_arrive_radius) / (k_heading_blend_dist - k_arrive_radius), 0.0f, 1.0f);
            desired_heading = wrap_angle(g1_heading + blend * wrap_angle(to_slot_heading - g1_heading));
        }
    }

    const bud::math::quaternion trunk_rot = robot->get_link_rotation("trunk_base");
    const bud::math::vec3 duck_up = trunk_rot * bud::math::vec3(0.0f, 1.0f, 0.0f);
    const bud::math::vec3 duck_fwd = trunk_rot * bud::math::vec3(1.0f, 0.0f, 0.0f);
    const float duck_current_heading = std::atan2(-duck_fwd.z, duck_fwd.x);
    const float heading_error = wrap_angle(desired_heading - duck_current_heading);
    const bool is_fallen = (duck_up.y < 0.40f);

    float target_vx = 0.0f;
    float target_vyaw = 0.0f;

    if (is_fallen) {
        fallen_timer += dt;
        cmd_vx = 0.0f;
        cmd_vyaw = 0.0f;

        // When knocked down for >1.5s, gracefully right the duck to standing posture
        constexpr float k_recovery_time = 1.5f;
        if (fallen_timer >= k_recovery_time) {
            auto* physics_scene = engine_ptr->get_physics_scene();
            if (physics_scene) {
                const bud::math::quaternion upright_rot =
                    glm::angleAxis(g1_heading, bud::math::vec3(0.0f, 1.0f, 0.0f));
                constexpr float k_sponza_ground_y = -0.0251f;
                const float spawn_y = std::max(current_position.y, k_sponza_ground_y) + 0.12f;
                const bud::math::vec3 upright_pos(current_position.x, spawn_y, current_position.z);
                physics_scene->get_world().set_articulation_link_transform(
                    robot->get_articulation_handle(), "trunk_base", upright_pos, upright_rot);
                if (policy_controller)
                    policy_controller->reset();
                bud::print("[Microduck] Righted from knockdown onto feet at ({:.2f}, {:.2f}, {:.2f})",
                           upright_pos.x, upright_pos.y, upright_pos.z);
            }
            fallen_timer = 0.0f;
        }
    } else {
        fallen_timer = 0.0f;

        if (dist > k_arrive_radius) {
            // Linear velocity proportional to distance, with a minimum threshold so the RL policy steps
            const float dist_error = dist - k_arrive_radius;
            const float raw_speed = std::clamp(dist_error * 0.8f + k_min_walk_speed, k_min_walk_speed, k_max_forward_speed);
            // Reduce forward speed when heading error is large, allowing duck to turn first
            const float align = std::clamp(std::cos(heading_error), 0.0f, 1.0f);
            target_vx = raw_speed * (align * align);
            target_vyaw = std::clamp(heading_error * 1.5f, -k_max_turn_rate, k_max_turn_rate);
        } else {
            target_vx = 0.0f;
            // In slot: align heading with G1 if slightly offset, but avoid micro-jitter
            if (std::abs(heading_error) > 0.15f)
                target_vyaw = std::clamp(heading_error * 1.2f, -k_max_turn_rate * 0.5f, k_max_turn_rate * 0.5f);
            else
                target_vyaw = 0.0f;
        }

        // Asymmetric slew-rate limiting: smooth start, gentle deceleration
        if (target_vx > cmd_vx)
            cmd_vx = std::min(target_vx, cmd_vx + k_max_accel * dt);
        else
            cmd_vx = std::max(target_vx, cmd_vx - k_max_decel * dt);

        cmd_vyaw = std::clamp(target_vyaw, cmd_vyaw - k_max_yaw_accel * dt, cmd_vyaw + k_max_yaw_accel * dt);
    }

    if (policy_controller) {
        policy_controller->set_twist(cmd_vx, 0.0f, cmd_vyaw);
        policy_controller->set_head(0.3491f, 0.25f, 0.0f, 0.0f);
        policy_controller->update(dt);
    }

    if (visual_bridge)
        visual_bridge->sync_transforms(*robot, engine_ptr->get_scene());
}

} // namespace bud::robots
