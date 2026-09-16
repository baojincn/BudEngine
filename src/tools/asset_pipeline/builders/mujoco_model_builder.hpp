#pragma once

// Cook step for the MuJoCo side of a robot asset.
//
// Runs MuJoCo's compiler offline and normalizes the URDF into MJCF, producing the payload stored in
// the robot .budasset as AssetChunkType::PhysicsModel. The runtime never parses a URDF: it compiles
// this MJCF and serves the meshes it references out of the existing visual assets.
//
// The asset carries Unitree's data only - inertials, joints, limits, dynamics, meshes and collision
// geometry exactly as their URDF states them - plus the per-joint motor model a URDF cannot express:
// armature, damping and Coulomb friction, and one torque motor per actuated joint (the URDF effort
// limit becomes the motor's ctrlrange). What is deliberately *not* baked:
//   - actuator gains (kp/kv): a controller/experiment choice, supplied by the runtime;
//   - solver and contact settings: experiment configuration, owned by the runtime.
// The one structural fixup is the free base joint: the URDF states the base as
// "world --floating--> root" and MuJoCo's importer drops that joint, which would weld the robot to
// the world. Restoring it is what the URDF asks for, not extra data.

#include <optional>
#include <string>

#include "src/robots/bud.robot.mujoco.hpp"
#include "src/robots/bud.robot.types.hpp"

namespace bud::asset_pipeline {

    struct MujocoCookOptions {
        // Kept for symmetry with other builders; nothing about the robot's physics is configurable
        // here on purpose.
        bool unused = false;
    };

    // Returns std::nullopt when the URDF cannot be compiled by MuJoCo; the caller should then keep
    // the asset without a PhysicsModel chunk and report a warning.
    std::optional<bud::robots::MujocoModelData> cook_mujoco_model(
        const std::string& urdf_path,
        const std::string& package_root,
        const bud::robots::RobotDef& robot_def,
        const MujocoCookOptions& options);

} // namespace bud::asset_pipeline
