// End to end check for the MuJoCo physics backend.
//
// It loads the *cooked* robot asset (no URDF, no loose files): the PhysicsModel chunk provides the
// normalized MJCF, the mesh bytes come out of our own mesh assets, and the MuJoCo backend compiles
// the whole world (floor + robot) into one model. It then steps the simulation and reports what the
// backend exposes: articulation state, joint angles, contacts and a raycast.
//
// Usage: mujoco_backend_test [robot_asset] [asset_root]

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "src/physics/bud.physics.world.hpp"
#include "src/physics/bud.physics.world.mujoco.hpp"
#include "src/robots/bud.robot.mujoco.hpp"
#include "src/robots/bud.robot.types.hpp"

namespace {

    constexpr double kPi = 3.14159265358979;

    float tilt_degrees(const bud::math::quaternion& rotation) {
        // Body local +Z (MuJoCo is Z-up) expressed in world, then its angle from world +Z.
        const float w = rotation.w;
        const float x = rotation.x;
        const float y = rotation.y;
        const float z = rotation.z;
        const float up_z = 1.0f - 2.0f * (x * x + y * y);
        return static_cast<float>(std::acos(std::clamp(up_z, -1.0f, 1.0f)) * 180.0 / kPi);
    }

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::filesystem::path asset_root =
        argc > 2 ? std::filesystem::path(argv[2])
                 : std::filesystem::path("D:/PersonalProjects/BudEngine/Content/Robots/g1_description");
    const std::filesystem::path robot_asset =
        argc > 1 ? std::filesystem::path(argv[1]) : asset_root / "g1_29dof.budasset";

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

    // Link and joint names come from the articulation chunk of the same asset, exactly like the
    // robot loader will do.
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

    // MuJoCo is Z-up: the floor is a thin box whose top face sits at z = 0.
    bud::physics::RigidBodyDesc floor_desc{};
    floor_desc.position = bud::math::vec3(0.0f, 0.0f, -0.1f);
    floor_desc.motion_type = bud::physics::MotionType::Static;
    floor_desc.shape.type = bud::physics::ShapeType::Box;
    floor_desc.shape.half_extent = bud::math::vec3(20.0f, 20.0f, 0.1f);
    floor_desc.material.friction = 0.8f;
    world.add_rigid_body(floor_desc);

    bud::physics::ArticulationDesc articulation{};
    articulation.name = robot_def->name;
    articulation.root_link = robot_def->root_link;
    articulation.root_position = bud::math::vec3(0.0f, 0.0f, 0.79f); // standing height
    articulation.root_rotation = bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);
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

    // Step 1.5 s and watch the robot.
    const float step_dt = 1.0f / 60.0f;
    for (int step = 0; step <= 90; ++step) {
        if (step > 0)
            world.step(step_dt);

        if (step % 15 != 0)
            continue;

        bud::physics::ArticulationStateSoA state;
        if (!world.get_articulation_state(handle, state))
            continue;
        const auto& root_position = state.link_positions.empty() ? articulation.root_position
                                                                 : state.link_positions[0];
        const auto& root_rotation = state.link_rotations.empty()
                                        ? bud::math::quaternion(1, 0, 0, 0)
                                        : state.link_rotations[0];
        std::printf("[test] t=%.2fs root_z=%.4f tilt=%.2f deg contacts=%zu knee=%.4f rad\n",
                    step * step_dt, root_position.y, tilt_degrees(root_rotation),
                    world.get_contacts().size(),
                    world.get_articulation_joint_angle(handle, "left_knee_joint"));
    }

    // Raycast straight down from above the robot: must hit the robot or the floor.
    const auto hit = world.raycast(bud::math::vec3(0.0f, 0.0f, 3.0f), bud::math::vec3(0.0f, 0.0f, -5.0f));
    std::printf("[test] raycast down: %s",
                hit ? "hit at " : "no hit\n");
    if (hit)
        std::printf("(%.3f, %.3f, %.3f)\n", hit->hit_point.x, hit->hit_point.y, hit->hit_point.z);

    std::printf("[test] DONE\n");
    return 0;
}
