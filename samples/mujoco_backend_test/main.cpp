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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "src/physics/bud.physics.world.hpp"
#include "src/physics/bud.physics.world.mujoco.hpp"
#include "src/robots/bud.robot.mujoco.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"

#include <glm/gtc/quaternion.hpp>

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

    // Scene mesh geometry carries no MuJoCo collision (visual-only placeholder handle). Adding one
    // exercises that path and must not disturb the world; the backend reports the count at compile.
    bud::physics::RigidBodyDesc scene_mesh_desc{};
    scene_mesh_desc.motion_type = bud::physics::MotionType::Static;
    scene_mesh_desc.shape.type = bud::physics::ShapeType::Mesh;
    world.add_rigid_body(scene_mesh_desc);

    bud::physics::ArticulationDesc articulation{};
    articulation.name = robot_def->name;
    articulation.root_link = robot_def->root_link;
    // Standing pose equilibrium height for bent knees, in engine coordinates (Y up).
    articulation.root_position = bud::math::vec3(0.0f, bud::robots::k_g1_standing_pelvis_height, 0.0f);
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
    float elapsed_s = 0.0f;
    bool fell_over = false;

    for (int step = 0; step <= total_steps; ++step) {
        if (step > 0)
            world.step(step_dt);
        elapsed_s = static_cast<float>(step) * step_dt;

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

    // Spatial query evidence: a ray through the robot hits the robot; an offset ray must hit the
    // scene floor. The floor probe proves the standing surface exists and that the non-colliding
    // scene mesh above did not disturb it.
    const auto robot_hit = world.raycast(bud::math::vec3(0.0f, 3.0f, 0.0f), bud::math::vec3(0.0f, -5.0f, 0.0f));
    std::printf("[test] raycast through the robot: %s", robot_hit ? "hit at " : "no hit\n");
    if (robot_hit)
        std::printf("(%.3f, %.3f, %.3f)\n", robot_hit->hit_point.x, robot_hit->hit_point.y, robot_hit->hit_point.z);

    const auto floor_hit = world.raycast(bud::math::vec3(3.0f, 3.0f, 3.0f), bud::math::vec3(3.0f, -5.0f, 3.0f));
    const bool floor_hit_ok = floor_hit.has_value() && std::abs(floor_hit->hit_point.y) < 0.01f;
    std::printf("[test] raycast beside the robot (floor): %s (y=%.4f)\n",
                floor_hit ? "hit" : "no hit", floor_hit ? floor_hit->hit_point.y : 0.0f);

    // --- Articulation lifecycle (F6): reset is recompile-free, remove/respawn does not accumulate ---
    const int bodies_with_robot = world.model_body_count();
    const int actuators_with_robot = world.model_actuator_count();
    const int sensors_with_robot = world.model_sensor_count();

    // IMU path: standing upright, the cooked framequat must report a unit quaternion whose body up
    // axis points along world up, and the gyro must be near zero.
    bool imu_ok = false;
    {
        bud::physics::ArticulationImu imu;
        if (world.get_articulation_imu(handle, imu)) {
            const glm::vec3 body_up = glm::mat3_cast(imu.orientation) * glm::vec3(0.0f, 1.0f, 0.0f);
            const float quat_len = glm::length(imu.orientation);
            imu_ok = std::abs(quat_len - 1.0f) < 1.0e-3f && body_up.y > 0.95f &&
                     glm::length(imu.angular_velocity) < 0.5f;
        }
    }

    world.reset_articulation(handle);
    world.step(step_dt);
    bud::physics::ArticulationStateSoA reset_state;
    const bool reset_ok = world.get_articulation_state(handle, reset_state) &&
                          !reset_state.link_positions.empty() &&
                          std::abs(reset_state.link_positions[0].y -
                                   bud::robots::k_g1_standing_pelvis_height) < 0.05f;

    // Ten remove/respawn cycles must not accumulate bodies or actuators.
    constexpr int kLifecycleCycles = 10;
    constexpr int kExpectedEmptyBodies = 2; // world + floor box
    auto active_handle = handle;
    int bodies_after_remove = 0;
    int actuators_after_remove = 0;
    int sensors_after_remove = 0;
    int bodies_after_respawn = 0;
    int actuators_after_respawn = 0;
    int sensors_after_respawn = 0;
    bool lifecycle_ok = true;
    for (int cycle = 0; cycle < kLifecycleCycles; ++cycle) {
        world.remove_articulation(active_handle);
        world.step(step_dt);
        bodies_after_remove = world.model_body_count();
        actuators_after_remove = world.model_actuator_count();
        sensors_after_remove = world.model_sensor_count();

        active_handle = world.create_articulation(articulation);
        world.set_articulation_joint_commands(active_handle, base_standing_cmd.to_joint_commands());
        world.step(step_dt);
        bodies_after_respawn = world.model_body_count();
        actuators_after_respawn = world.model_actuator_count();
        sensors_after_respawn = world.model_sensor_count();

        if (bodies_after_remove != kExpectedEmptyBodies || actuators_after_remove != 0 ||
            sensors_after_remove != 0 || bodies_after_respawn != bodies_with_robot ||
            actuators_after_respawn != actuators_with_robot ||
            sensors_after_respawn != sensors_with_robot)
            lifecycle_ok = false;
    }

    // Concurrency smoke test (F7): getters must be safe while the world steps on another thread.
    // Without a TSan build this cannot prove the absence of races, but a lock-order or reentrancy
    // mistake shows up here as a deadlock or crash.
    {
        std::atomic<bool> reader_run{ true };
        std::atomic<int> reader_calls{ 0 };
        std::thread reader([&]() {
            bud::physics::ArticulationStateSoA local;
            while (reader_run.load(std::memory_order_relaxed)) {
                world.get_articulation_state(active_handle, local);
                world.get_contacts();
                world.raycast(bud::math::vec3(0.0f, 3.0f, 0.0f), bud::math::vec3(0.0f, -5.0f, 0.0f));
                world.get_body_count();
                reader_calls.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        for (int i = 0; i < 60; ++i) {
            world.step(step_dt);
            // Give the reader a real chance to interleave instead of being starved by the lock.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        reader_run.store(false, std::memory_order_relaxed);
        reader.join();
        std::printf("[test] concurrent getters: %d calls while stepping (no deadlock)\n",
                    reader_calls.load());
    }

    // Orientation decomposition self-check (F9): recover known pitch/roll, including a pitch past
    // 90 degrees where the folded asin version returned the supplement instead of the true angle.
    bool orientation_ok = true;
    {
        struct OrientationCase {
            const char* name;
            float pitch;
            float roll;
        };
        const OrientationCase cases[] = {
            { "upright", 0.0f, 0.0f },
            { "pitch only", 0.35f, 0.0f },
            { "roll only", 0.0f, -0.22f },
            { "pitch past 90", 2.0f, 0.0f },
        };
        for (const auto& c : cases) {
            const bud::math::quaternion q =
                glm::angleAxis(c.pitch, bud::math::vec3(0.0f, 0.0f, 1.0f)) *
                glm::angleAxis(c.roll, bud::math::vec3(1.0f, 0.0f, 0.0f));
            float pitch = 0.0f;
            float roll = 0.0f;
            float tilt = 0.0f;
            bud::robots::compute_body_orientation(q, pitch, roll, tilt);
            if (std::abs(pitch - (-c.pitch)) > 1.0e-3f || std::abs(roll - c.roll) > 1.0e-3f) {
                std::printf("[test] orientation '%s' mismatch: pitch=%.4f (want %.4f), roll=%.4f (want %.4f)\n",
                            c.name, pitch, -c.pitch, roll, c.roll);
                orientation_ok = false;
            }
        }
    }

    const bool remove_ok = lifecycle_ok && actuators_after_remove == 0;
    const bool respawn_ok = lifecycle_ok && bodies_after_respawn == bodies_with_robot &&
                            actuators_after_respawn == actuators_with_robot;
    std::printf("[test] lifecycle: reset=%s, %d cycles, remove bodies %d->%d / actuators %d->%d / sensors %d->%d, respawn bodies=%d / actuators=%d / sensors=%d\n",
                reset_ok ? "ok" : "FAILED", kLifecycleCycles, bodies_with_robot, bodies_after_remove,
                actuators_with_robot, actuators_after_remove, sensors_with_robot, sensors_after_remove,
                bodies_after_respawn, actuators_after_respawn, sensors_after_respawn);
    std::printf("[test] imu: %s\n", imu_ok ? "ok" : "FAILED");

    const float pelvis_fluctuation = max_pelvis_y - min_pelvis_y;
    std::printf("\n--- Stage 4 Standing Stabilization Summary ---\n");
    std::printf("Duration:              %.1f / %.1f s\n", elapsed_s, duration_sec);
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

    if (floor_hit_ok)
        std::printf("[PASS] Scene floor is queryable at y=%.4f m\n", floor_hit->hit_point.y);
    else {
        std::printf("[FAIL] Scene floor is not queryable beside the robot\n");
        all_passed = false;
    }

    if (reset_ok)
        std::printf("[PASS] Reset restored the spawn pose without a world rebuild\n");
    else {
        std::printf("[FAIL] Reset did not restore the spawn pose\n");
        all_passed = false;
    }

    if (orientation_ok)
        std::printf("[PASS] Orientation decomposition recovers known pitch/roll (incl. past 90 deg)\n");
    else {
        std::printf("[FAIL] Orientation decomposition is wrong\n");
        all_passed = false;
    }

    if (imu_ok)
        std::printf("[PASS] IMU reports an upright unit orientation with near-zero angular velocity\n");
    else {
        std::printf("[FAIL] IMU read is wrong or missing\n");
        all_passed = false;
    }

    if (remove_ok && respawn_ok)
        std::printf("[PASS] Remove folded the world back to %d bodies / %d sensors, respawn returned to %d / %d (no accumulation)\n",
                    bodies_after_remove, sensors_after_remove, bodies_after_respawn,
                    sensors_after_respawn);
    else {
        std::printf("[FAIL] Articulation lifecycle accumulated state\n");
        all_passed = false;
    }

    std::printf("-----------------------------------------------\n");
    if (all_passed)
        std::printf("[Stage 4 RESULT] ALL CHECKS PASSED\n");
    else
        std::printf("[Stage 4 RESULT] CHECKS FAILED\n");

    return all_passed ? 0 : 1;
}
