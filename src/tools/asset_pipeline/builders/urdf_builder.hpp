#pragma once

#include "src/robots/bud.robot.types.hpp"
#include <string>

namespace bud::asset_pipeline {

struct UrdfBuildOptions {
    std::string package_root = "";       // Package root for package:// URIs (auto-detected if empty)
    int max_convex_vertices = 128;      // Vertex cap for convex hulls (matches Isaac Sim physics standard)
    float scale = 0.0f;                  // Scale factor override (0.0f = auto-detect CAD millimeters to meters)
    bool dump_json = true;               // Output human-readable JSON definition alongside .budasset
    bool use_cache = true;
    // Source URDF, set by cook_urdf. The MuJoCo physics model is cooked from it (MuJoCo is the only
    // handler of robot physics data); cooking from an already built RobotDef alone leaves the robot
    // asset without a PhysicsModel chunk.
    std::string source_urdf_path = "";
};

class UrdfBuilder {
public:
    // Builds a complete robot URDF into modular .budasset mesh containers and a master AssetType::Articulation .budasset container
    static bool build_urdf(
        const std::string& urdf_path,
        const std::string& output_robot_dir,
        const UrdfBuildOptions& options = {}
    ) {
        return cook_urdf(urdf_path, output_robot_dir, options);
    }

    static bool cook_urdf(
        const std::string& urdf_path,
        const std::string& output_robot_dir,
        const UrdfBuildOptions& options = {}
    );

    // Builds a RobotDef into modular .budasset mesh containers and a master AssetType::Articulation .budasset container
    static bool build_robot(
        bud::robot::RobotDef robot_def,
        const std::string& output_robot_dir,
        const UrdfBuildOptions& options = {}
    ) {
        return cook_robot(std::move(robot_def), output_robot_dir, options);
    }

    // Cooks a RobotDef into modular .budasset mesh containers and a master AssetType::Articulation .budasset container
    static bool cook_robot(
        bud::robot::RobotDef robot_def,
        const std::string& output_robot_dir,
        const UrdfBuildOptions& options = {}
    );

    // Optional legacy helper: cooks unified visual geometry of a robot
    static bool cook_robot_to_single_asset(
        const bud::robot::RobotDef& robot_def,
        const std::string& package_root,
        const std::string& output_budasset_path
    );
};

// Backward-compatible aliases
using UrdfCooker = UrdfBuilder;
using UrdfCookOptions = UrdfBuildOptions;

} // namespace bud::asset_pipeline
