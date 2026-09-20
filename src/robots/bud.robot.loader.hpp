#pragma once

#include "src/robots/bud.robot.types.hpp"
#include "src/physics/bud.physics.scene.hpp"
#include <span>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

namespace JPH {
    class Ragdoll;
    class PhysicsSystem;
    class TwoBodyConstraint;
}

namespace bud::robots {

struct LowCmd;

struct RobotSpawnParams {
    bud::math::vec3 position{ 0.0f, 0.0f, 0.0f };
    bud::math::quaternion rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    bool activate = true;
    bool enable_motors = true;
    float default_motor_stiffness = 500.0f; // Kp (Spring frequency / stiffness)
    float default_motor_damping = 50.0f;    // Kd (Damping)
    float default_motor_max_torque = 100.0f;// Max effort (Nm)
    std::string asset_path = "";
    std::unordered_map<std::string, float> initial_joint_angles;
};

class RobotInstance {
public:
    RobotInstance(const bud::robots::RobotDef& def,
                  JPH::Ragdoll* ragdoll,
                  JPH::PhysicsSystem* system,
                  std::unordered_map<std::string, int> link_to_part,
                  std::unordered_map<std::string, int> joint_to_constraint);

    RobotInstance(const bud::robots::RobotDef& def,
                  physics::PhysicsScene* scene,
                  physics::ArticulationHandle articulation_handle,
                  std::unordered_map<std::string, int> link_to_part);

    ~RobotInstance();

    const bud::robots::RobotDef& get_definition() const { return m_def; }
    JPH::Ragdoll* get_ragdoll() const { return m_ragdoll; }

    bool is_simulation() const {
        return m_articulation_handle.is_valid();
    }
    physics::ArticulationHandle get_articulation_handle() const {
        return m_articulation_handle;
    }

    // Drive joint angle with PD position motor
    void set_joint_target_angle(const std::string& joint_name, float target_angle_rad);
    
    // Drive joint angular velocity
    void set_joint_target_velocity(const std::string& joint_name, float target_velocity_rad_s);

    // Drive multiple joints with custom PD gains & torque feedforward
    void set_joint_commands(std::span<const physics::JointCommand> commands);

    // Drive robot with complete LowCmd
    void set_low_cmd(const LowCmd& cmd);

    // Get current joint angle in radians
    float get_joint_angle(const std::string& joint_name) const;

    // Get world position/orientation of a link
    bud::math::vec3 get_link_position(const std::string& link_name) const;
    bud::math::quaternion get_link_rotation(const std::string& link_name) const;

    struct LinkTransform {
        std::string link_name;
        bud::math::vec3 position;
        bud::math::quaternion rotation;
    };
    std::vector<LinkTransform> get_all_link_transforms() const;

    // Remove robot bodies & constraints from physics simulation
    void remove_from_simulation();

    int get_part_index(const std::string& link_name) const {
        auto it = m_link_to_part.find(link_name);
        if (it != m_link_to_part.end())
            return it->second;
        return -1;
    }

private:
    bud::robots::RobotDef m_def;
    JPH::Ragdoll* m_ragdoll = nullptr;
    JPH::PhysicsSystem* m_system = nullptr;
    physics::PhysicsScene* m_physics_scene = nullptr;
    physics::ArticulationHandle m_articulation_handle{};
    std::unordered_map<std::string, int> m_link_to_part;
    std::unordered_map<std::string, int> m_joint_to_constraint;
};

class RobotLoader {
public:
    // Builds and spawns a robot from RobotDef into Jolt physics using Ragdoll + MotorSettings
    static std::unique_ptr<RobotInstance> spawn_robot(
        physics::PhysicsScene& scene,
        const bud::robots::RobotDef& robot_def,
        const RobotSpawnParams& params = {}
    );

    // Convenience loader from an Articulation .budasset
    static std::unique_ptr<RobotInstance> spawn_robot_from_file(
        physics::PhysicsScene& scene,
        const std::string& robot_file_path,
        const RobotSpawnParams& params = {}
    );
};

} // namespace bud::robots

namespace bud::physics {
    using bud::robots::RobotSpawnParams;
    using bud::robots::RobotInstance;
    using bud::robots::RobotLoader;
}
