#pragma once

// PolicySpec: a configuration file that maps an external ONNX policy onto this engine's robot.
//
// External locomotion policies do not share an observation/action layout: dimension, ordering,
// scaling, whether normalization is baked into the graph, the joint order and even the control
// frequency all differ. Rather than freezing one layout, the runtime reads a spec that names the
// signals it must assemble and the joint order it must produce. Changing policies then means
// changing a JSON file, not the engine.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace bud::rl {

    // Observation signals the runtime can assemble. A spec lists them in the exact order the policy
    // expects its input vector.
    enum class ObsSignal {
        BaseAngVel,       // 3, angular velocity in the base body frame
        ProjectedGravity, // 3, gravity direction in the base body frame
        BaseLinVel,       // 3, linear velocity in the base body frame (privileged)
        Command,          // 3, (vx, vy, yaw_rate)
        JointPosRel,      // J, joint position minus the default pose
        JointPos,         // J, raw joint position
        JointVel,         // J, joint velocity
        JointTorque,      // J, joint torque
        LastAction,       // J, previous action
        Phase,            // 2, sin/cos of a periodic gait phase
    };

    // Which axes a base-frame signal is expressed in. External policies are trained against the
    // robot's own (URDF) body axes; the engine's own frame is a fixed remap of those.
    enum class ObsFrame {
        Robot,
        Engine,
    };

    const char* obs_signal_name(ObsSignal signal);
    std::optional<ObsSignal> obs_signal_from_name(const std::string& name);
    int obs_signal_dim(ObsSignal signal, size_t joint_count);

    struct ObsTerm {
        ObsSignal signal = ObsSignal::JointPosRel;
        ObsFrame frame = ObsFrame::Robot; // only meaningful for base_ang_vel / projected_gravity
        std::vector<float> scale;         // empty = 1; one entry = uniform; otherwise per component
        float period = 0.8f;              // gait phase period in seconds (Phase signal only)
        int history = 1;                  // frames stacked per term, oldest first
    };

    // Optional dense policy network: a flat float32 weight file plus its dimensions. Used when the
    // engine should run the policy without ONNX Runtime (see DensePolicy).
    struct NetworkSpec {
        std::string type;    // "mlp" or "lstm_mlp"; empty means no dense network
        std::string weights; // weight file path
        int input_size = 0;
        int hidden_size = 0;   // lstm_mlp only
        int actor_hidden = 0;  // lstm_mlp only
        int output_size = 0;
        int layers = 1;        // lstm_mlp only
        // mlp only: either an explicit [in, h..., out] chain, or hidden_sizes to build one.
        std::vector<int> layer_sizes;
        std::vector<int> hidden_sizes;
    };

    struct PolicySpec {
        std::string name = "policy";
        float policy_dt = 0.02f; // 50 Hz by default
        std::vector<ObsTerm> obs_terms;
        // Empty means normalization is already baked into the ONNX graph.
        std::vector<float> normalization_mean;
        std::vector<float> normalization_std;
        std::vector<std::string> joint_order; // policy output/input joint order
        std::string default_pose = "g1_standing";
        std::vector<float> default_pose_values; // resolved, in policy order; empty = derive from name
        float action_scale = 0.25f;
        float action_clip_min = -1.0f;
        float action_clip_max = 1.0f;
        bool command_from_joystick = true;
        // Optional per-joint gain overrides (policy joint order). Empty = derive from the robot
        // definition / G1 gain table. External policies usually ship their own kp/kd.
        std::vector<float> action_kp;
        std::vector<float> action_kd;
        // Joints the policy does not command (a 12-DOF leg policy leaves the waist and arms free).
        // Holding them stiffly approximates the fixed torso such policies are trained against; a
        // compliant torso moves in ways the policy cannot observe and destabilises the gait.
        // Zero kp disables freezing and leaves those joints to the robot definition's gains.
        float freeze_other_kp = 2000.0f;
        float freeze_other_kd = 50.0f;
        NetworkSpec network; // empty type = use the ONNX runner (or the null policy)

        static std::optional<PolicySpec> load_json(const std::string& path, std::string& error);
        bool validate(size_t joint_count, std::string& error) const;
        int observation_dim(size_t joint_count) const;
    };

} // namespace bud::rl
