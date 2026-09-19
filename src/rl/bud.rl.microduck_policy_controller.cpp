#include "src/rl/bud.rl.microduck_policy_controller.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include "src/core/bud.logger.hpp"

namespace bud::rl {

namespace {

constexpr const char* k_policy_joint_names[MicroduckPolicyController::ACTION_DIM] = {
    "left_hip_yaw", "left_hip_roll", "left_hip_pitch", "left_knee", "left_ankle",
    "neck_pitch", "head_pitch", "head_yaw", "head_roll",
    "right_hip_yaw", "right_hip_roll", "right_hip_pitch", "right_knee", "right_ankle"
};

constexpr float k_default_pose[MicroduckPolicyController::ACTION_DIM] = {
    0.0f,     // left_hip_yaw
    -0.0873f, // left_hip_roll
    -0.4579f, // left_hip_pitch
    -0.0049f, // left_knee
    0.4530f,  // left_ankle
    0.3491f,  // neck_pitch
    0.3491f,  // head_pitch
    0.0f,     // head_yaw
    0.0f,     // head_roll
    0.0f,     // right_hip_yaw
    0.0873f,  // right_hip_roll
    0.4579f,  // right_hip_pitch
    0.0049f,  // right_knee
    -0.4530f  // right_ankle
};

constexpr float k_default_kp = 0.55f;
constexpr float k_default_kd = 0.053f;
constexpr float k_action_scale = 1.0f;

// Settle phase: the duck spawns slightly above the ground and its servos are soft (kp 0.55), so
// handing over immediately fed the policy joint velocities of +-1.8 rad/s and an out-of-distribution
// observation it could never recover from. Hold the default pose stiffly for long enough that the
// body is at rest, then let the policy take over.
constexpr int k_settle_steps = 75; // 1.5 s at 50 Hz
constexpr float k_hold_kp = 4.0f;
constexpr float k_hold_kd = 0.4f;

} // namespace

MicroduckPolicyController::MicroduckPolicyController() = default;
MicroduckPolicyController::~MicroduckPolicyController() = default;

bool MicroduckPolicyController::initialize(
    physics::PhysicsWorldBase& in_world,
    physics::ArticulationHandle handle,
    const bud::robots::RobotDef& robot_def,
    const std::string& onnx_path,
    std::string& error
) {
    world = &in_world;
    articulation = handle;
    ready = false;
    last_error.clear();

    if (!handle.is_valid()) {
        error = "invalid articulation handle passed to MicroduckPolicyController";
        return false;
    }

    std::unordered_map<std::string, int> asset_joint_map;
    for (size_t i = 0; i < robot_def.joints.size(); ++i)
        asset_joint_map[robot_def.joints[i].name] = static_cast<int>(i);

    joint_names.resize(ACTION_DIM);
    policy_to_asset.resize(ACTION_DIM);
    default_pose.resize(ACTION_DIM);
    kp.resize(ACTION_DIM, k_default_kp);
    kd.resize(ACTION_DIM, k_default_kd);
    limit_lower.resize(ACTION_DIM, -3.14159f);
    limit_upper.resize(ACTION_DIM, 3.14159f);
    command_buffer.resize(ACTION_DIM);

    for (size_t i = 0; i < ACTION_DIM; ++i) {
        joint_names[i] = k_policy_joint_names[i];
        default_pose[i] = k_default_pose[i];

        const auto it = asset_joint_map.find(joint_names[i]);
        if (it == asset_joint_map.end()) {
            error = "Microduck robot asset missing expected policy joint: " + joint_names[i];
            return false;
        }
        policy_to_asset[i] = it->second;

        const bud::robots::JointDef& j_def = robot_def.joints[it->second];
        if (j_def.motor.enabled && j_def.motor.stiffness > 0.0f) {
            kp[i] = j_def.motor.stiffness;
            kd[i] = j_def.motor.damping;
        }
        if (j_def.limit.lower < j_def.limit.upper) {
            limit_lower[i] = j_def.limit.lower;
            limit_upper[i] = j_def.limit.upper;
        }

        command_buffer[i].joint_name = joint_names[i];
        command_buffer[i].q = default_pose[i];
        command_buffer[i].dq = 0.0f;
        command_buffer[i].kp = kp[i];
        command_buffer[i].kd = kd[i];
        command_buffer[i].tau_ff = 0.0f;
    }

    if (!onnx_path.empty()) {
        // The policy is an MLP with an input normalizer. Prefer the exported dense weights: ONNX
        // Runtime on this toolchain terminates the process on graphs that contain initializers.
        const std::filesystem::path onnx_file(onnx_path);
        const std::filesystem::path weights_path =
            onnx_file.parent_path() / (onnx_file.stem().string() + "_weights.bin");
        const std::filesystem::path norm_path =
            onnx_file.parent_path() / (onnx_file.stem().string() + "_norm.bin");

        if (std::filesystem::exists(weights_path) && std::filesystem::exists(norm_path)) {
            DensePolicy::Layout layout = DensePolicy::make_mlp(
                { static_cast<int>(OBS_DIM), 512, 256, 128, static_cast<int>(ACTION_DIM) });
            std::string dense_error;
            if (!dense_policy.load(weights_path.string(), layout, dense_error)) {
                error = dense_error;
                return false;
            }

            std::ifstream norm_in(norm_path, std::ios::binary | std::ios::ate);
            if (!norm_in) {
                error = "cannot open the Microduck normalizer: " + norm_path.string();
                return false;
            }
            const std::streamsize norm_bytes = norm_in.tellg();
            norm_in.seekg(0, std::ios::beg);
            if (static_cast<size_t>(norm_bytes) != 2 * OBS_DIM * sizeof(float)) {
                error = "the Microduck normalizer has the wrong size: " + norm_path.string();
                return false;
            }
            std::vector<float> norm(2 * OBS_DIM);
            norm_in.read(reinterpret_cast<char*>(norm.data()), norm_bytes);
            obs_mean.assign(norm.begin(), norm.begin() + static_cast<std::ptrdiff_t>(OBS_DIM));
            obs_std.assign(norm.begin() + static_cast<std::ptrdiff_t>(OBS_DIM), norm.end());
            use_dense = true;
            bud::print("[MicroduckPolicy] dense policy ready (obs={}, action={}, weights='{}')", OBS_DIM,
                       ACTION_DIM, weights_path.string());
        } else {
            std::string runner_err;
            if (!runner.load(onnx_path, runner_err)) {
                error = runner_err;
                return false;
            }
            if (runner.input_size() != static_cast<int>(OBS_DIM)) {
                error = "Microduck policy expects input dim " + std::to_string(runner.input_size()) +
                        ", but controller provides " + std::to_string(OBS_DIM);
                return false;
            }
            if (runner.output_size() != static_cast<int>(ACTION_DIM)) {
                error = "Microduck policy outputs dim " + std::to_string(runner.output_size()) +
                        ", but controller expects " + std::to_string(ACTION_DIM);
                return false;
            }
            bud::print("[MicroduckPolicy] ONNX runtime path in use; export '{}' and '{}' to avoid it",
                       weights_path.string(), norm_path.string());
        }
    }

    world->set_articulation_joint_commands(articulation, command_buffer);

    ready = true;
    return true;
}

void MicroduckPolicyController::update(float dt) {
    if (!ready || !world)
        return;

    accumulator += dt;
    constexpr int k_max_substeps = 4;
    int substeps = 0;

    while (accumulator >= POLICY_DT && substeps < k_max_substeps) {
        step_policy();
        accumulator -= POLICY_DT;
        ++substeps;
        ++step_count;
    }

    if (substeps == k_max_substeps)
        accumulator = 0.0f;
}

void MicroduckPolicyController::reset() {
    accumulator = 0.0f;
    step_count = 0;
    observation.fill(0.0f);
    action.fill(0.0f);
    last_action.fill(0.0f);

    for (size_t i = 0; i < ACTION_DIM; ++i) {
        command_buffer[i].q = default_pose[i];
        command_buffer[i].dq = 0.0f;
        command_buffer[i].kp = kp[i];
        command_buffer[i].kd = kd[i];
        command_buffer[i].tau_ff = 0.0f;
    }
    if (world && ready)
        world->set_articulation_joint_commands(articulation, command_buffer);
}

void MicroduckPolicyController::step_policy() {
    if (step_count < static_cast<uint64_t>(k_settle_steps)) {
        for (size_t i = 0; i < ACTION_DIM; ++i) {
            command_buffer[i].q = default_pose[i];
            command_buffer[i].dq = 0.0f;
            command_buffer[i].kp = k_hold_kp;
            command_buffer[i].kd = k_hold_kd;
            command_buffer[i].tau_ff = 0.0f;
        }
        world->set_articulation_joint_commands(articulation, command_buffer);
        last_action.fill(0.0f);
        return;
    }

    observation.fill(0.0f);

    physics::ArticulationImu imu{};
    const bool has_imu = world->get_articulation_imu(articulation, imu);
    if (has_imu) {
        observation[0] = imu.angular_velocity.x;
        observation[1] = imu.angular_velocity.y;
        observation[2] = imu.angular_velocity.z;

        const glm::vec3 gravity_world(0.0f, 0.0f, -1.0f);
        const glm::vec3 projected_gravity = glm::conjugate(imu.orientation_robot) * gravity_world;
        observation[3] = projected_gravity.x;
        observation[4] = projected_gravity.y;
        observation[5] = projected_gravity.z;
    } else {
        observation[3] = 0.0f;
        observation[4] = 0.0f;
        observation[5] = -1.0f;
    }

    // One state query per policy step instead of per-joint lookups, and the joint velocities are the
    // real ones: feeding the policy dq = 0 makes it blind to how fast the joints are moving, which
    // is enough to make it thrash.
    physics::ArticulationStateSoA state;
    const bool has_state = world->get_articulation_state(articulation, state);
    for (size_t i = 0; i < ACTION_DIM; ++i) {
        const int asset_index = policy_to_asset[i];
        float q = 0.0f;
        float dq = 0.0f;
        if (asset_index >= 0 && has_state) {
            const size_t index = static_cast<size_t>(asset_index);
            if (index < state.joint_positions.size())
                q = state.joint_positions[index];
            if (index < state.joint_velocities.size())
                dq = state.joint_velocities[index];
        }
        observation[6 + i] = q - default_pose[i];
        observation[20 + i] = dq;
        observation[34 + i] = last_action[i];
    }

    observation[48] = current_command.twist.x;
    observation[49] = current_command.twist.y;
    observation[50] = current_command.twist.z;

    observation[51] = current_command.head[0];
    observation[52] = current_command.head[1];
    observation[53] = current_command.head[2];
    observation[54] = current_command.head[3];

    observation[55] = 0.0f;
    observation[56] = 0.0f;
    observation[57] = current_command.body_z;
    observation[58] = current_command.body_roll;
    observation[59] = current_command.body_pitch;
    observation[60] = 0.0f;

    if (use_dense) {
        for (size_t i = 0; i < OBS_DIM; ++i)
            observation[i] = (observation[i] - obs_mean[i]) / obs_std[i];
        std::string run_err;
        if (!dense_policy.run(observation.data(), nullptr, action.data(), run_err)) {
            bud::eprint("[MicroduckPolicy] dense evaluation error: {}", run_err);
            return;
        }
    } else if (runner.is_loaded()) {
        std::string run_err;
        if (!runner.run(observation.data(), action.data(), run_err)) {
            bud::eprint("[MicroduckPolicy] ONNX evaluation error: {}", run_err);
            return;
        }
    } else {
        action.fill(0.0f);
    }

    last_action = action;

    for (size_t i = 0; i < ACTION_DIM; ++i) {
        float q_target = default_pose[i] + action[i] * k_action_scale;
        q_target = std::clamp(q_target, limit_lower[i], limit_upper[i]);

        command_buffer[i].q = q_target;
        command_buffer[i].dq = 0.0f;
        // Keep the stiffer hold gains during the policy phase. Dropping from the 4.0 hold to the
        // model's authored 0.55 the instant the policy took over made the legs go soft and the duck
        // collapsed before the policy could act.
        command_buffer[i].kp = k_hold_kp;
        command_buffer[i].kd = k_hold_kd;
        command_buffer[i].tau_ff = 0.0f;
    }

    world->set_articulation_joint_commands(articulation, command_buffer);
}

} // namespace bud::rl
