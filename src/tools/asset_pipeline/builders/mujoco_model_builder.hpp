#pragma once

// Cook step for the MuJoCo side of a robot asset.
//
// Runs MuJoCo's compiler offline, normalizes the URDF into MJCF (free base joint, actuators,
// armature, solver/contact options) and produces the payload that is stored in the robot .budasset
// as AssetChunkType::PhysicsModel. The runtime never parses a URDF: it compiles this MJCF and serves
// the meshes it references out of the existing visual assets.

#include <string>
#include <vector>

#include "src/robots/bud.robot.mujoco.hpp"
#include "src/robots/bud.robot.types.hpp"

namespace bud::asset_pipeline {

    // Actuator gains and armature per joint group. Not in URDF: armature (rotor inertia) belongs to
    // the motor, and position gains are a controller choice. These are the sim-to-real knobs.
    struct MujocoJointGroup {
        std::string token;        // joint-name substring, e.g. "knee"
        double kp = 40.0;         // N*m/rad
        double kv = 2.0;          // N*m/(rad/s)
        double armature = 0.0;    // kg*m^2 (rotor inertia reflected to the joint)
        double max_torque = 0.0;  // 0 = take the URDF effort limit
        double frictionloss = -1.0; // >=0 overrides the URDF joint friction
    };

    struct MujocoCookOptions {
        std::vector<MujocoJointGroup> joint_groups;
        int max_hull_vertices = 32;  // MuJoCo mesh collision hull budget
        double timestep = 0.002;     // physics timestep recorded in the model
        int solver_iterations = 100;
        bool contact_noslip = true;  // dry friction solver pass (robotics quality contacts)
    };

    // Returns std::nullopt when the URDF cannot be compiled by MuJoCo; the caller should then keep
    // the asset without a PhysicsModel chunk and report a warning.
    std::optional<bud::robots::MujocoModelData> cook_mujoco_model(
        const std::string& urdf_path,
        const std::string& package_root,
        const bud::robots::RobotDef& robot_def,
        const MujocoCookOptions& options);

} // namespace bud::asset_pipeline
