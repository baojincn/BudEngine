#pragma once

// Runs an external ONNX locomotion policy against a robot articulation:
//   observation (per PolicySpec) -> ONNX graph -> action -> joint targets -> existing LowCmd PD.
//
// When no ONNX path is given the controller still runs and emits zero actions, which holds the
// default pose. That "null policy" mode is how the observation/action plumbing can be verified
// without a trained model, and it is also a safe fallback.

#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "src/physics/bud.physics.world.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/rl/bud.rl.dense_policy.hpp"
#include "src/rl/bud.rl.observation.hpp"
#include "src/rl/bud.rl.policy_runner.hpp"
#include "src/rl/bud.rl.policy_spec.hpp"

namespace bud::rl {

    struct PolicyControllerConfig {
        bool enabled = false;
        std::string onnx_path; // empty = null policy (zero action, holds the default pose)
        std::string spec_path; // required when enabled
    };

    class G1PolicyController {
    public:
        bool initialize(physics::PhysicsWorldBase& world,
                        physics::ArticulationHandle handle,
                        const bud::robots::RobotDef& robot_def,
                        const PolicyControllerConfig& config,
                        std::string& error);

        bool ready() const { return is_ready; }
        bool has_network() const { return runner.is_loaded() || dense_policy.is_loaded(); }
        const std::string& last_error() const { return error_message; }

        void set_command(const glm::vec3& command) { command_vector = command; }
        const glm::vec3& command() const { return command_vector; }

        void update(float dt);
        void reset();

        const PolicySpec& spec() const { return policy_spec; }
        int observation_dim() const;
        int action_dim() const { return static_cast<int>(action.size()); }
        unsigned long long steps() const { return step_count; }

    private:
        void step_policy();

        physics::PhysicsWorldBase* world = nullptr;
        physics::ArticulationHandle articulation{};
        std::string error_message;

        PolicySpec policy_spec;
        PolicyRunner runner; // ONNX path
        DensePolicy dense_policy; // dependency-free weight-file path
        ObservationBuilder observation_builder; // holds the per-term history buffers
        bool is_ready = false;

        // Policy joint i -> index into the articulation's joint arrays (asset order).
        std::vector<int> policy_to_asset;
        std::vector<std::string> joint_names_policy;
        std::vector<float> default_pose_policy;
        std::vector<float> kp_policy;
        std::vector<float> kd_policy;

        std::vector<float> joint_pos_policy;
        std::vector<float> joint_vel_policy;
        std::vector<float> joint_torque_policy;
        std::vector<float> last_action;
        std::vector<float> observation;
        std::vector<float> action;
        std::vector<float> recurrent_state; // opaque LSTM/GRU state; empty for feed-forward policies
        std::vector<physics::JointCommand> command_buffer;

        // Gait diagnostics: the ankle link heights over a telemetry window tell a real step from a
        // skid (a skidding robot keeps both feet near the ground while the base translates).
        int left_foot_link = -1;
        int right_foot_link = -1;
        float foot_min_y = 1.0e9f;
        float foot_max_y = -1.0e9f;

        glm::vec3 command_vector{ 0.0f };
        float accumulator = 0.0f;
        float policy_time = 0.0f; // seconds since reset, drives Phase terms
        unsigned long long step_count = 0;
    };

} // namespace bud::rl
