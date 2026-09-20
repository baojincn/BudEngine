#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "src/core/bud.asset.types.hpp"

namespace bud::robots {

constexpr uint64_t ARTICULATION_CHUNK_MAGIC = 0x544F424F52445542ULL;
constexpr uint32_t ARTICULATION_CHUNK_VERSION = 1;

struct ConvexHullData {
    std::vector<float> points;          // 3 floats per vertex (x, y, z)
    std::vector<uint32_t> indices;      // Triangle indices (3 per face)
    float aabb_min[3] = { 0.0f, 0.0f, 0.0f };
    float aabb_max[3] = { 0.0f, 0.0f, 0.0f };
    float center_of_mass[3] = { 0.0f, 0.0f, 0.0f };
    float volume = 0.0f;
};

struct InertialDef {
    float origin_xyz[3] = { 0.0f, 0.0f, 0.0f };
    float origin_rpy[3] = { 0.0f, 0.0f, 0.0f };
    float mass = 0.0f;
    float inertia[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }; // ixx, ixy, ixz, iyy, iyz, izz
};

enum class GeometryType : uint8_t {
    Mesh = 0,
    Box = 1,
    Sphere = 2,
    Cylinder = 3,
    Capsule = 4
};

struct GeometryDef {
    GeometryType type = GeometryType::Mesh;
    std::string mesh_path;              // Relative mirror path, e.g. "meshes/visual/pelvis.budasset"
    float scale[3] = { 1.0f, 1.0f, 1.0f };
    float box_size[3] = { 0.0f, 0.0f, 0.0f };
    float sphere_radius = 0.0f;
    float cylinder_radius = 0.0f;
    float cylinder_length = 0.0f;
    float capsule_radius = 0.0f;
    float capsule_length = 0.0f;
};

struct VisualDef {
    std::string name;
    float origin_xyz[3] = { 0.0f, 0.0f, 0.0f };
    float origin_rpy[3] = { 0.0f, 0.0f, 0.0f };
    GeometryDef geometry;
    float color_rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string material_name;
};

struct CollisionDef {
    std::string name;
    float origin_xyz[3] = { 0.0f, 0.0f, 0.0f };
    float origin_rpy[3] = { 0.0f, 0.0f, 0.0f };
    GeometryDef geometry;
    ConvexHullData convex_hull;
};

struct LinkDef {
    std::string name;
    InertialDef inertial;
    std::vector<VisualDef> visuals;
    std::vector<CollisionDef> collisions;
};

enum class JointType : uint8_t {
    Revolute = 0,
    Continuous = 1,
    Prismatic = 2,
    Fixed = 3,
    Floating = 4,
    Planar = 5
};

struct JointLimit {
    float lower = 0.0f;
    float upper = 0.0f;
    float effort = 0.0f;                // Max torque / force (Nm / N)
    float velocity = 0.0f;              // Max velocity (rad/s or m/s)
};

struct JointDynamics {
    float damping = 0.0f;
    float friction = 0.0f;
};

struct JointMotor {
    bool enabled = false;
    float stiffness = 0.0f;             // Kp (Spring frequency / stiffness)
    float damping = 0.0f;               // Kd
    float max_force = 0.0f;
};

struct JointDef {
    std::string name;
    JointType type = JointType::Fixed;
    std::string parent_link;
    std::string child_link;
    float origin_xyz[3] = { 0.0f, 0.0f, 0.0f };
    float origin_rpy[3] = { 0.0f, 0.0f, 0.0f };
    float axis[3] = { 0.0f, 0.0f, 1.0f };
    JointLimit limit;
    JointDynamics dynamics;
    JointMotor motor;
};

struct RobotDef {
    std::string name;
    std::string root_link;
    std::vector<LinkDef> links;
    std::vector<JointDef> joints;

    const LinkDef* find_link(const std::string& link_name) const;
    LinkDef* find_link(const std::string& link_name);

    const JointDef* find_joint(const std::string& joint_name) const;
    JointDef* find_joint(const std::string& joint_name);

    std::vector<const JointDef*> get_child_joints(const std::string& link_name) const;
    const JointDef* get_parent_joint(const std::string& link_name) const;

    std::vector<uint8_t> serialize_binary() const;
    static std::optional<RobotDef> deserialize_binary(const uint8_t* data, size_t size);

    bool save_json(const std::string& path) const;
    static std::optional<RobotDef> load_from_budasset(const std::string& path);
};

} // namespace bud::robots

namespace bud {
    namespace robot = robots;
}
