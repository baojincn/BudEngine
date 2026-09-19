#pragma once

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <glm/glm.hpp>

#include "src/physics/bud.physics.world.hpp"
#include "src/robots/bud.robot.types.hpp"
#include "src/rl/bud.rl.dense_policy.hpp"
#include "src/rl/bud.rl.policy_runner.hpp"

namespace bud::rl {

struct MicroduckCommand {
    glm::vec3 twist{ 0.0f }; // vx, vy, vyaw
    std::array<float, 4> head{ 0.0f, 0.0f, 0.0f, 0.0f }; // neck_pitch, head_pitch, head_yaw, head_roll
    float body_z = 0.0f;
    float body_roll = 0.0f;
    float body_pitch = 0.0f;
};

class MicroduckPolicyController {
public:
    static constexpr size_t OBS_DIM = 61;
    static constexpr size_t ACTION_DIM = 14;
    static constexpr float POLICY_DT = 0.02f; // 50 Hz

    MicroduckPolicyController();
    ~MicroduckPolicyController();

    bool initialize(
        physics::PhysicsWorldBase& world,
        physics::ArticulationHandle handle,
        const bud::robots::RobotDef& robot_def,
        const std::string& onnx_path,
        std::string& error
    );

    bool is_ready() const { return ready; }
    void set_command(const MicroduckCommand& command) { current_command = command; }
    void set_twist(float vx, float vy, float vyaw) {
        current_command.twist = glm::vec3(vx, vy, vyaw);
    }
    void set_head(float neck_pitch, float head_pitch, float head_yaw, float head_roll) {
        current_command.head = { neck_pitch, head_pitch, head_yaw, head_roll };
    }

    const MicroduckCommand& command() const { return current_command; }
    void update(float dt);
    void reset();

private:
    void step_policy();

    physics::PhysicsWorldBase* world = nullptr;
    physics::ArticulationHandle articulation{};
    bool ready = false;
    std::string last_error;

    // The published Microduck policy is a plain MLP with an input normalizer, so it runs through the
    // dependency-free dense runtime. ONNX Runtime stays as a fallback only; on this toolchain it
    // terminates on graphs that contain initializers.
    DensePolicy dense_policy;
    PolicyRunner runner;
    bool use_dense = false;
    std::vector<float> obs_mean;
    std::vector<float> obs_std;
    MicroduckCommand current_command{};

    float accumulator = 0.0f;
    uint64_t step_count = 0;

    std::vector<std::string> joint_names;
    std::vector<int> policy_to_asset;
    std::vector<float> default_pose;
    std::vector<float> kp;
    std::vector<float> kd;
    std::vector<float> limit_lower;
    std::vector<float> limit_upper;

    std::array<float, OBS_DIM> observation{};
    std::array<float, ACTION_DIM> action{};
    std::array<float, ACTION_DIM> last_action{};

    std::vector<physics::JointCommand> command_buffer;

};

} // namespace bud::rl
