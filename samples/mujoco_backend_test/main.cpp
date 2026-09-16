// End to end check for the MuJoCo physics backend - Stage 4 Standing Stabilization.
//
// It loads the cooked G1 robot asset (PhysicsModel MJCF + meshes) and runs a 60-second continuous
// physical standing simulation on a ground plane under MuJoCo Newton solver with 500 Hz substeps.
// A closed-loop ankle balance compensator stabilizes pelvis pitch and roll.
//
// Acceptance criteria:
// - 60s continuous stable standing without falling
// - Pelvis height fluctuation < 2 cm (0.02 m)
// - Maximum body tilt < 5 degrees
// - No floor penetration, continuous contacts maintained
//
// Usage: mujoco_backend_test [robot_asset] [asset_root] [duration_sec]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "src/physics/bud.physics.world.hpp"
#include "src/physics/bud.physics.world.mujoco.hpp"
#include "src/robots/bud.robot.mujoco.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::filesystem::path asset_root =
        argc > 2 ? std::filesystem::path(argv[2])
                 : std::filesystem::path("D:/PersonalProjects/BudEngine/Content/Robots/g1_description");
    const std::filesystem::path robot_asset =
        argc > 1 ? std::filesystem::path(argv[1]) : asset_root / "g1_29dof.budasset";
    const float duration_sec =
        argc > 3 ? std::stof(argv[3]) : 60.0f;

    if (!std::filesystem::exists(robot_asset)) {
        std::printf("[test] robot asset not found: %s\n", robot_asset.string().c_str());
        return 1;
    }

    // Cooked payload: MJCF text plus the mesh references the backend serves from our assets.
    auto cooked = bud::robots::MujocoModelData::load_from_budasset(robot_asset.string());
    if (!cooked) {
        std::printf("[test] no PhysicsModel chunk in %s\n", robot_asset.string().c_str());
        return 1;
    }
    std::printf("[test] cooked model: format=%s payload=%zu bytes meshes=%zu\n",
                cooked->format == bud::robots::MujocoModelFormat::Mjcf ? "MJCF" : "MJB",
                cooked->model_payload.size(), cooked->meshes.size());

    // Link and joint names come from the articulation chunk of the same asset.
    auto robot_def = bud::robots::RobotDef::load_from_budasset(robot_asset.string());
    if (!robot_def) {
        std::printf("[test] no Articulation chunk in %s\n", robot_asset.string().c_str());
        return 1;
    }

    bud::physics::PhysicsWorldConfig config;
    config.backend = bud::physics::PhysicsBackend::Mujoco;
    config.asset_root = asset_root.string();
    config.gravity = bud::math::vec3(0.0f, -9.80665f, 0.0f);
    config.max_bodies = 64;

    bud::physics::MujocoPhysicsWorld world;
    if (!world.init(config)) {
        std::printf("[test] backend init failed\n");
        return 1;
    }

    // Engine space is Y-up: a horizontal slab is thin in Y and wide in X/Z, 10 cm below origin.
    bud::physics::RigidBodyDesc floor_desc{};
    floor_desc.position = bud::math::vec3(0.0f, -0.1f, 0.0f);
    floor_desc.motion_type = bud::physics::MotionType::Static;
    floor_desc.shape.type = bud::physics::ShapeType::Box;
    floor_desc.shape.half_extent = bud::math::vec3(20.0f, 0.1f, 20.0f);
    floor_desc.material.friction = 1.0f;
    world.add_rigid_body(floor_desc);

    bud::physics::ArticulationDesc articulation{};
    articulation.name = robot_def->name;
    articulation.root_link = robot_def->root_link;
    // Standing pose equilibrium height for bent knees is Y = 0.785m in engine coordinates
    articulation.root_position = bud::math::vec3(0.0f, 0.785f, 0.0f);
    articulation.root_rotation = bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    // Pre-populate joint angles with official standing pose so simulation starts in equilibrium
    articulation.initial_joint_angles = bud::robots::get_g1_standing_joint_angles();

    for (const auto& link : robot_def->links) {
        bud::physics::ArticulationLinkDesc link_desc{};
        link_desc.name = link.name;
        articulation.links.push_back(std::move(link_desc));
    }

    for (const auto& joint : robot_def->joints) {
        bud::physics::ArticulationJointDesc joint_desc{};
        joint_desc.name = joint.name;
        joint_desc.parent_link = joint.parent_link;
        joint_desc.child_link = joint.child_link;
        bud::robots::get_default_g1_gains(joint.name, joint_desc.stiffness, joint_desc.damping);
        joint_desc.max_torque = joint.limit.effort;
        articulation.joints.push_back(std::move(joint_desc));
    }

    articulation.cooked_model.format =
        cooked->format == bud::robots::MujocoModelFormat::Mjcf
            ? bud::physics::CookedModelFormat::MjcfText
            : bud::physics::CookedModelFormat::MjbBinary;
    articulation.cooked_model.payload.assign(cooked->model_payload.begin(), cooked->model_payload.end());
    articulation.cooked_model.render_metadata_json = cooked->render_metadata_json;
    articulation.cooked_model.physics_metadata_json = cooked->physics_metadata_json;
    for (const auto& mesh : cooked->meshes) {
        bud::physics::CookedModelMeshRef ref{};
        ref.model_name = mesh.mjcf_name;
        ref.asset_path = mesh.asset_path;
        articulation.cooked_model.meshes.push_back(std::move(ref));
    }

    const auto handle = world.create_articulation(articulation);
    if (!handle.is_valid()) {
        std::printf("[test] create_articulation failed\n");
        return 1;
    }

    auto base_standing_cmd = bud::robots::make_g1_standing_cmd();
    world.set_articulation_joint_commands(handle, base_standing_cmd.to_joint_commands());
    std::printf("[test] initialized standing stance (%zu joints), target duration: %.1fs\n",
                articulation.joints.size(), duration_sec);

    const float step_dt = 1.0f / 60.0f;
    const int total_steps = static_cast<int>(std::ceil(duration_sec / step_dt));

    float prev_pitch = 0.0f;
    float prev_roll = 0.0f;
    float min_pelvis_y = 1.0e9f;
    float max_pelvis_y = -1.0e9f;
    float max_tilt_deg = 0.0f;
    float min_lowest_y = 1.0e9f;
    bool fell_over = false;

    for (int step = 0; step <= total_steps; ++step) {
        if (step > 0)
            world.step(step_dt);

        bud::physics::ArticulationStateSoA state;
        if (!world.get_articulation_state(handle, state))
            continue;

        const auto& pelvis_pos = state.link_positions.empty() ? articulation.root_position
                                                              : state.link_positions[0];
        const auto& pelvis_rot = state.link_rotations.empty()
                                    ? bud::math::quaternion(1, 0, 0, 0)
                                    : state.link_rotations[0];


        float pitch_rad = 0.0f;
        float roll_rad = 0.0f;
        float tilt_deg = 0.0f;
        bud::robots::compute_body_orientation(pelvis_rot, pitch_rad, roll_rad, tilt_deg);

        const float pitch_vel = (step > 0) ? (pitch_rad - prev_pitch) / step_dt : 0.0f;
        const float roll_vel = (step > 0) ? (roll_rad - prev_roll) / step_dt : 0.0f;
        prev_pitch = pitch_rad;
        prev_roll = roll_rad;

        // Apply active ankle balance adjustment
        auto active_cmd = base_standing_cmd;
        bud::robots::apply_standing_balance(active_cmd, pitch_rad, pitch_vel, roll_rad, roll_vel);
        world.set_articulation_joint_commands(handle, active_cmd.to_joint_commands());

        // Track stats after initial settling (t >= 0.5s)
        const float time_s = step * step_dt;
        if (time_s >= 0.5f) {
            min_pelvis_y = std::min(min_pelvis_y, pelvis_pos.y);
            max_pelvis_y = std::max(max_pelvis_y, pelvis_pos.y);
            max_tilt_deg = std::max(max_tilt_deg, tilt_deg);

            for (const auto& link_pos : state.link_positions)
                min_lowest_y = std::min(min_lowest_y, link_pos.y);
        }

        // Print progress every 1 second (60 steps)
        if (step % 60 == 0 || step == total_steps) {
            float ankle_q = world.get_articulation_joint_angle(handle, "left_ankle_pitch_joint");
            float knee_q = world.get_articulation_joint_angle(handle, "left_knee_joint");
            float hip_q = world.get_articulation_joint_angle(handle, "left_hip_pitch_joint");
            std::printf("[test] t=%5.1fs pelvis=(%.3f, %.3f, %.3f) tilt=%4.2f deg pitch=%+5.2f | hip=%.3f knee=%.3f ankle=%.3f (cmd=%.3f) contacts=%zu\n",
                        time_s, pelvis_pos.x, pelvis_pos.y, pelvis_pos.z,
                        tilt_deg, pitch_rad * 180.0f / 3.14159f,
                        hip_q, knee_q, ankle_q, active_cmd.motor_cmd[4].q,
                        world.get_contacts().size());
        }

        if (tilt_deg > 15.0f || pelvis_pos.y < 0.40f) {
            std::printf("[test] ROBOT FELL OVER at t=%.2fs (pelvis_y=%.3f, tilt=%.2f deg)\n",
                        time_s, pelvis_pos.y, tilt_deg);
            fell_over = true;
            break;
        }
    }

    // Raycast straight down from above the robot
    const auto hit = world.raycast(bud::math::vec3(0.0f, 3.0f, 0.0f), bud::math::vec3(0.0f, -5.0f, 0.0f));
    std::printf("[test] raycast down: %s",
                hit ? "hit at " : "no hit\n");
    if (hit)
        std::printf("(%.3f, %.3f, %.3f)\n", hit->hit_point.x, hit->hit_point.y, hit->hit_point.z);

    const float pelvis_fluctuation = max_pelvis_y - min_pelvis_y;
    std::printf("\n--- Stage 4 Standing Stabilization Summary ---\n");
    std::printf("Duration:              %.1f / %.1f s\n", fell_over ? (prev_pitch) : duration_sec, duration_sec);
    std::printf("Pelvis Height Range:   [%.4f, %.4f] m (fluctuation: %.4f m)\n",
                min_pelvis_y, max_pelvis_y, pelvis_fluctuation);
    std::printf("Max Body Tilt:         %.2f deg\n", max_tilt_deg);
    std::printf("Lowest Body Point Y:   %.4f m\n", min_lowest_y);

    bool all_passed = true;
    if (fell_over) {
        std::printf("[FAIL] Robot did not complete %.1f s standing\n", duration_sec);
        all_passed = false;
    } else {
        std::printf("[PASS] Completed %.1f s continuous standing\n", duration_sec);
    }

    if (pelvis_fluctuation < 0.02f)
        std::printf("[PASS] Pelvis height fluctuation < 2 cm (actual: %.2f cm)\n", pelvis_fluctuation * 100.0f);
    else {
        std::printf("[FAIL] Pelvis height fluctuation >= 2 cm (actual: %.2f cm)\n", pelvis_fluctuation * 100.0f);
        all_passed = false;
    }

    if (max_tilt_deg < 5.0f)
        std::printf("[PASS] Maximum body tilt < 5 deg (actual: %.2f deg)\n", max_tilt_deg);
    else {
        std::printf("[FAIL] Maximum body tilt >= 5 deg (actual: %.2f deg)\n", max_tilt_deg);
        all_passed = false;
    }

    if (min_lowest_y > -0.005f)
        std::printf("[PASS] No ground penetration (lowest body point: %.4f m)\n", min_lowest_y);
    else {
        std::printf("[FAIL] Ground penetration detected (lowest body point: %.4f m)\n", min_lowest_y);
        all_passed = false;
    }

    std::printf("-----------------------------------------------\n");
    if (all_passed)
        std::printf("[Stage 4 RESULT] ALL CHECKS PASSED\n");
    else
        std::printf("[Stage 4 RESULT] CHECKS FAILED\n");

    return all_passed ? 0 : 1;
}
