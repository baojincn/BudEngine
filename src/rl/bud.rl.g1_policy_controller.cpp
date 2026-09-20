#include "src/rl/bud.rl.g1_policy_controller.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

#include "src/core/bud.logger.hpp"
#include "src/robots/bud.robot.lowcmd.hpp"
#include "src/rl/bud.rl.observation.hpp"

namespace bud::rl {

    bool G1PolicyController::initialize(physics::PhysicsWorldBase& in_world,
                                        physics::ArticulationHandle handle,
                                        const bud::robots::RobotDef& robot_def,
                                        const PolicyControllerConfig& config,
                                        std::string& error) {
        world = &in_world;
        articulation = handle;
        is_ready = false;
        error_message.clear();

        if (!config.enabled) {
            error = "policy controller was initialised while disabled";
            return false;
        }
        if (config.spec_path.empty()) {
            error = "a policy controller needs a spec file (--policy-spec)";
            return false;
        }

        std::string spec_error;
        auto loaded_spec = PolicySpec::load_json(config.spec_path, spec_error);
        if (!loaded_spec) {
            error = spec_error;
            return false;
        }
        policy_spec = std::move(*loaded_spec);

        if (!policy_spec.validate(robot_def.joints.size(), spec_error)) {
            error = spec_error;
            return false;
        }

        // Map the policy's joint order onto the articulation's joint arrays. A missing joint is a
        // configuration error that must stop here: silently driving a subset is how a policy ends up
        // commanding the wrong limbs.
        std::unordered_map<std::string, int> asset_joint_index;
        for (size_t i = 0; i < robot_def.joints.size(); ++i)
            asset_joint_index[robot_def.joints[i].name] = static_cast<int>(i);

        const size_t joint_count = policy_spec.joint_order.size();
        policy_to_asset.resize(joint_count);
        joint_names_policy = policy_spec.joint_order;
        kp_policy.resize(joint_count, 0.0f);
        kd_policy.resize(joint_count, 0.0f);
        for (size_t i = 0; i < joint_count; ++i) {
            const auto it = asset_joint_index.find(policy_spec.joint_order[i]);
            if (it == asset_joint_index.end()) {
                error = "policy joint_order names '" + policy_spec.joint_order[i] +
                        "', which is not a joint of this robot";
                return false;
            }
            policy_to_asset[i] = it->second;

            const bud::robots::JointDef& joint = robot_def.joints[it->second];
            if (joint.motor.enabled) {
                kp_policy[i] = joint.motor.stiffness;
                kd_policy[i] = joint.motor.damping;
            } else {
                bud::robots::get_default_g1_gains(joint.name, kp_policy[i], kd_policy[i]);
            }
        }

        // Default pose: explicit array wins, otherwise resolve the named pose, otherwise zero.
        const auto standing_pose = bud::robots::get_g1_standing_joint_angles();
        default_pose_policy.assign(joint_count, 0.0f);
        for (size_t i = 0; i < joint_count; ++i) {
            if (!policy_spec.default_pose_values.empty())
                default_pose_policy[i] = policy_spec.default_pose_values[i];
            else {
                const auto it = standing_pose.find(policy_spec.joint_order[i]);
                if (it != standing_pose.end())
                    default_pose_policy[i] = it->second;
            }
        }

        // Hold the joints the policy does not command. A 12-DOF leg policy leaves the waist and arms
        // out of its observation, so a compliant torso moves in ways it cannot compensate for; a stiff
        // hold approximates the fixed torso of the model it was trained on.
        if (policy_spec.freeze_other_kp > 0.0f) {
            const std::unordered_set<std::string> policy_joints(policy_spec.joint_order.begin(),
                                                                policy_spec.joint_order.end());
            std::vector<physics::JointCommand> frozen;
            for (const bud::robots::JointDef& joint : robot_def.joints) {
                if (joint.type == bud::robots::JointType::Fixed)
                    continue;
                if (policy_joints.count(joint.name) > 0)
                    continue;
                physics::JointCommand command;
                command.joint_name = joint.name;
                const auto pose_it = standing_pose.find(joint.name);
                command.q = (pose_it != standing_pose.end()) ? pose_it->second : 0.0f;
                command.dq = 0.0f;
                command.kp = policy_spec.freeze_other_kp;
                command.kd = policy_spec.freeze_other_kd;
                command.tau_ff = 0.0f;
                frozen.push_back(std::move(command));
            }
            if (!frozen.empty()) {
                world->set_articulation_joint_commands(articulation, frozen);
                bud::print("[RL] holding {} joints the policy does not command (kp={}, kd={})",
                           frozen.size(), policy_spec.freeze_other_kp, policy_spec.freeze_other_kd);
            }
        }

        {
            std::unordered_map<std::string, int> link_index;
            for (size_t i = 0; i < robot_def.links.size(); ++i)
                link_index[robot_def.links[i].name] = static_cast<int>(i);
            const auto left = link_index.find("left_ankle_roll_link");
            const auto right = link_index.find("right_ankle_roll_link");
            left_foot_link = (left != link_index.end()) ? left->second : -1;
            right_foot_link = (right != link_index.end()) ? right->second : -1;
        }

        joint_pos_policy.assign(joint_count, 0.0f);
        joint_vel_policy.assign(joint_count, 0.0f);
        joint_torque_policy.assign(joint_count, 0.0f);
        last_action.assign(joint_count, 0.0f);
        action.assign(joint_count, 0.0f);
        command_buffer.resize(joint_count);

        std::string builder_error;
        if (!observation_builder.initialize(policy_spec, joint_count, builder_error)) {
            error = builder_error;
            return false;
        }
        const int expected_obs = observation_builder.observation_dim();
        observation.resize(static_cast<size_t>(expected_obs));

        // A dense weight-file network takes precedence: it needs neither ONNX Runtime nor protobuf,
        // which also makes it the portable choice for non-x86/NVIDIA deployment targets.
        if (!policy_spec.network.type.empty()) {
            DensePolicy::Layout layout;
            if (policy_spec.network.type == "mlp") {
                std::vector<int> sizes = policy_spec.network.layer_sizes;
                if (sizes.empty()) {
                    sizes.push_back(policy_spec.network.input_size);
                    for (int hidden : policy_spec.network.hidden_sizes)
                        sizes.push_back(hidden);
                    sizes.push_back(policy_spec.network.output_size);
                }
                layout = DensePolicy::make_mlp(std::move(sizes));
            } else {
                layout = DensePolicy::make_lstm_mlp(policy_spec.network.input_size,
                                                    policy_spec.network.hidden_size,
                                                    policy_spec.network.actor_hidden,
                                                    policy_spec.network.output_size);
                layout.layers = policy_spec.network.layers;
            }

            if (layout.input_size != expected_obs) {
                error = "network.input_size " + std::to_string(layout.input_size) +
                        " does not match the observation the spec assembles (" +
                        std::to_string(expected_obs) + ")";
                return false;
            }
            if (layout.output_size != static_cast<int>(joint_count)) {
                error = "network.output_size " + std::to_string(layout.output_size) +
                        " does not match the policy joint count (" + std::to_string(joint_count) + ")";
                return false;
            }

            std::string dense_error;
            if (!dense_policy.load(policy_spec.network.weights, layout, dense_error)) {
                error = dense_error;
                return false;
            }
            recurrent_state.assign(static_cast<size_t>(dense_policy.state_size()), 0.0f);
            bud::print("[RL] dense policy ready: obs={} action={} state={} weights='{}'",
                       layout.input_size, layout.output_size, dense_policy.state_size(),
                       policy_spec.network.weights);
        } else if (!config.onnx_path.empty()) {
            std::string runner_error;
            if (!runner.load(config.onnx_path, runner_error)) {
                error = runner_error;
                return false;
            }
            if (runner.input_size() != expected_obs) {
                error = "policy expects " + std::to_string(runner.input_size()) +
                        " observation values but the spec assembles " + std::to_string(expected_obs);
                return false;
            }
            if (runner.output_size() != static_cast<int>(joint_count)) {
                error = "policy outputs " + std::to_string(runner.output_size()) +
                        " values but the spec lists " + std::to_string(joint_count) + " joints";
                return false;
            }
            recurrent_state.assign(static_cast<size_t>(runner.state_size()), 0.0f);
            bud::print("[RL] policy ready: obs={} action={} state={}", runner.input_size(),
                       runner.output_size(), runner.state_size());
        }

        // Explicit per-joint gains win over the robot definition / G1 table.
        if (!policy_spec.action_kp.empty()) {
            for (size_t i = 0; i < joint_count; ++i)
                kp_policy[i] = policy_spec.action_kp[i];
        }
        if (!policy_spec.action_kd.empty()) {
            for (size_t i = 0; i < joint_count; ++i)
                kd_policy[i] = policy_spec.action_kd[i];
        }

        is_ready = true;
        error.clear();
        return true;
    }

    int G1PolicyController::observation_dim() const {
        return static_cast<int>(observation.size());
    }

    void G1PolicyController::update(float dt) {
        if (!is_ready || dt <= 0.0f)
            return;
        accumulator += dt;
        // A single frame can cover more than one policy period when the frame rate dips; run them in
        // sequence so the control loop stays at the trained rate.
        int guard = 0;
        while (accumulator >= policy_spec.policy_dt && guard < 8) {
            accumulator -= policy_spec.policy_dt;
            step_policy();
            ++guard;
        }
        if (guard >= 8)
            accumulator = 0.0f; // give up on catching up rather than spiral
    }

    void G1PolicyController::step_policy() {
        physics::ArticulationStateSoA state;
        if (!world->get_articulation_state(articulation, state))
            return;

        physics::ArticulationImu imu;
        const bool has_imu = world->get_articulation_imu(articulation, imu);

        for (size_t i = 0; i < policy_to_asset.size(); ++i) {
            const int asset_index = policy_to_asset[i];
            if (asset_index < 0)
                continue;
            const size_t index = static_cast<size_t>(asset_index);
            if (index < state.joint_positions.size())
                joint_pos_policy[i] = state.joint_positions[index];
            if (index < state.joint_velocities.size())
                joint_vel_policy[i] = state.joint_velocities[index];
            if (index < state.joint_torques.size())
                joint_torque_policy[i] = state.joint_torques[index];
        }

        // Engine-frame base quantities (Y up, matched to the rest of the engine).
        glm::quat engine_rotation(1.0f, 0.0f, 0.0f, 0.0f);
        if (!state.link_rotations.empty())
            engine_rotation = state.link_rotations[0];
        const glm::vec3 gravity_engine =
            glm::conjugate(engine_rotation) * glm::vec3(0.0f, -1.0f, 0.0f);
        glm::vec3 ang_vel_engine(0.0f);
        if (!state.link_angular_velocities.empty())
            ang_vel_engine = glm::conjugate(engine_rotation) * state.link_angular_velocities[0];
        glm::vec3 lin_vel_engine(0.0f);
        if (!state.link_linear_velocities.empty())
            lin_vel_engine = glm::conjugate(engine_rotation) * state.link_linear_velocities[0];

        // Robot (URDF) frame quantities, which is what external policies are trained on.
        glm::vec3 gravity_robot = gravity_engine;
        glm::vec3 ang_vel_robot = ang_vel_engine;
        glm::vec3 lin_vel_robot = lin_vel_engine;
        if (has_imu) {
            gravity_robot = glm::conjugate(imu.orientation_robot) * glm::vec3(0.0f, 0.0f, -1.0f);
            ang_vel_robot = imu.angular_velocity;
            lin_vel_robot = imu.linear_velocity;
        }

        ObservationInput input;
        input.joint_pos = joint_pos_policy.data();
        input.joint_vel = joint_vel_policy.data();
        input.joint_torque = joint_torque_policy.data();
        input.last_action = last_action.data();
        input.default_pose = default_pose_policy.data();
        input.joint_count = policy_to_asset.size();
        input.base_ang_vel = ang_vel_robot;
        input.projected_gravity = gravity_robot;
        input.base_lin_vel = lin_vel_robot;
        input.base_ang_vel_engine = ang_vel_engine;
        input.projected_gravity_engine = gravity_engine;
        input.base_lin_vel_engine = lin_vel_engine;
        input.command = command_vector;
        input.policy_time = policy_time;

        std::string obs_error;
        if (!observation_builder.build(policy_spec, input, observation, obs_error)) {
            error_message = obs_error;
            return;
        }

        if (dense_policy.is_loaded()) {
            std::string run_error;
            const bool is_recurrent = dense_policy.state_size() > 0;
            if (!dense_policy.run(observation.data(),
                                  is_recurrent ? recurrent_state.data() : nullptr, action.data(),
                                  run_error)) {
                error_message = run_error;
                return;
            }
        } else if (runner.is_loaded()) {
            std::string run_error;
            const bool ran = runner.state_size() > 0
                                 ? runner.run_stepped(observation.data(), recurrent_state.data(),
                                                      action.data(), run_error)
                                 : runner.run(observation.data(), action.data(), run_error);
            if (!ran) {
                error_message = run_error;
                return;
            }
        } else {
            // Null policy: zero action => hold the default pose.
            std::fill(action.begin(), action.end(), 0.0f);
        }

        for (size_t i = 0; i < action.size(); ++i)
            action[i] = std::clamp(action[i], policy_spec.action_clip_min, policy_spec.action_clip_max);

        const float action_scale = policy_spec.action_scale;
        for (size_t i = 0; i < policy_to_asset.size(); ++i) {
            physics::JointCommand& command = command_buffer[i];
            command.joint_name = joint_names_policy[i];
            command.q = default_pose_policy[i] + action[i] * action_scale;
            command.dq = 0.0f;
            command.kp = kp_policy[i];
            command.kd = kd_policy[i];
            command.tau_ff = 0.0f;
        }
        world->set_articulation_joint_commands(articulation, command_buffer);

        if (step_count == 0) {
            bud::print("[RL] first policy step: obs={} action={} state={} cmd=({:.2f},{:.2f},{:.2f}) "
                       "q[0]={:.3f} kp[0]={:.1f} kd[0]={:.2f} gravity_robot=({:.2f},{:.2f},{:.2f})",
                       static_cast<int>(observation.size()), static_cast<int>(action.size()),
                       static_cast<int>(recurrent_state.size()), command_vector.x, command_vector.y,
                       command_vector.z, command_buffer.empty() ? 0.0f : command_buffer[0].q,
                       kp_policy.empty() ? 0.0f : kp_policy[0], kd_policy.empty() ? 0.0f : kd_policy[0],
                       gravity_robot.x, gravity_robot.y, gravity_robot.z);
        }

        // Periodic telemetry so a headless run can be checked for actual motion, not just survival.
        // foot_swing is the ankle-height range over the window: a few centimetres means the policy is
        // stepping; near zero while the base moves means it is skidding.
        for (int foot : { left_foot_link, right_foot_link }) {
            if (foot < 0 || static_cast<size_t>(foot) >= state.link_positions.size())
                continue;
            const float foot_y = state.link_positions[static_cast<size_t>(foot)].y;
            foot_min_y = std::min(foot_min_y, foot_y);
            foot_max_y = std::max(foot_max_y, foot_y);
        }
        if (step_count > 0 && step_count % 100 == 0) {
            const glm::vec3 base_position = state.link_positions.empty() ? glm::vec3(0.0f)
                                                                         : state.link_positions[0];
            bud::print("[RL] t={:.1f}s base=({:.3f},{:.3f},{:.3f}) cmd=({:.2f},{:.2f},{:.2f}) "
                       "foot_swing={:.3f}m",
                       policy_time, base_position.x, base_position.y, base_position.z,
                       command_vector.x, command_vector.y, command_vector.z,
                       (foot_max_y > foot_min_y) ? (foot_max_y - foot_min_y) : 0.0f);
            foot_min_y = 1.0e9f;
            foot_max_y = -1.0e9f;
        }

        last_action = action;
        policy_time += policy_spec.policy_dt;
        ++step_count;
    }

    void G1PolicyController::reset() {
        if (!is_ready)
            return;
        world->reset_articulation(articulation);
        std::fill(last_action.begin(), last_action.end(), 0.0f);
        std::fill(action.begin(), action.end(), 0.0f);
        std::fill(recurrent_state.begin(), recurrent_state.end(), 0.0f);
        observation_builder.reset();
        accumulator = 0.0f;
        policy_time = 0.0f;
        step_count = 0;
    }

} // namespace bud::rl
