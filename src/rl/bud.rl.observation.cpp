#include "src/rl/bud.rl.observation.hpp"

#include <cmath>

namespace bud::rl {

    namespace {
        void apply_scale(std::vector<float>& values, const std::vector<float>& scale) {
            if (scale.empty())
                return;
            for (size_t i = 0; i < values.size(); ++i) {
                if (scale.size() == 1)
                    values[i] *= scale[0];
                else if (i < scale.size())
                    values[i] *= scale[i];
            }
        }
    } // namespace

    bool build_term(const ObsTerm& term, const ObservationInput& input, std::vector<float>& out,
                    std::string& error) {
        out.clear();
        switch (term.signal) {
            case ObsSignal::BaseAngVel:
            case ObsSignal::ProjectedGravity:
            case ObsSignal::BaseLinVel: {
                const bool engine = term.frame == ObsFrame::Engine;
                const glm::vec3& value = term.signal == ObsSignal::BaseAngVel
                                             ? (engine ? input.base_ang_vel_engine : input.base_ang_vel)
                                         : term.signal == ObsSignal::ProjectedGravity
                                             ? (engine ? input.projected_gravity_engine
                                                       : input.projected_gravity)
                                             : (engine ? input.base_lin_vel_engine : input.base_lin_vel);
                out = { value.x, value.y, value.z };
                break;
            }
            case ObsSignal::Command:
                out = { input.command.x, input.command.y, input.command.z };
                break;
            case ObsSignal::Phase: {
                const float phase = std::fmod(input.policy_time, term.period) / term.period;
                const float angle = phase * 6.28318530718f;
                out = { std::sin(angle), std::cos(angle) };
                break;
            }
            case ObsSignal::JointPos: {
                if (input.joint_count == 0 || input.joint_pos == nullptr) {
                    error = "joint_pos was requested but no joint state is available";
                    return false;
                }
                out.assign(input.joint_pos, input.joint_pos + input.joint_count);
                break;
            }
            case ObsSignal::JointPosRel: {
                if (input.joint_count == 0 || input.joint_pos == nullptr || input.default_pose == nullptr) {
                    error = "joint_pos_rel needs both joint state and a default pose";
                    return false;
                }
                out.resize(input.joint_count);
                for (size_t i = 0; i < input.joint_count; ++i)
                    out[i] = input.joint_pos[i] - input.default_pose[i];
                break;
            }
            case ObsSignal::JointVel: {
                if (input.joint_count == 0 || input.joint_vel == nullptr) {
                    error = "joint_vel was requested but no joint velocity is available";
                    return false;
                }
                out.assign(input.joint_vel, input.joint_vel + input.joint_count);
                break;
            }
            case ObsSignal::JointTorque: {
                if (input.joint_count == 0 || input.joint_torque == nullptr) {
                    error = "joint_torque was requested but no joint torque is available";
                    return false;
                }
                out.assign(input.joint_torque, input.joint_torque + input.joint_count);
                break;
            }
            case ObsSignal::LastAction: {
                if (input.joint_count == 0 || input.last_action == nullptr) {
                    error = "last_action was requested but no previous action is available";
                    return false;
                }
                out.assign(input.last_action, input.last_action + input.joint_count);
                break;
            }
        }
        apply_scale(out, term.scale);
        return true;
    }

    bool ObservationBuilder::initialize(const PolicySpec& spec, size_t joint_count, std::string& error) {
        term_history.clear();
        term_history.resize(spec.obs_terms.size());
        dim = spec.observation_dim(joint_count);
        if (dim <= 0) {
            error = "the spec assembles a zero-length observation";
            return false;
        }
        primed = false;
        return true;
    }

    void ObservationBuilder::reset() {
        for (auto& history : term_history)
            history.clear();
        primed = false;
    }

    bool ObservationBuilder::build(const PolicySpec& spec, const ObservationInput& input,
                                   std::vector<float>& out, std::string& error) {
        out.clear();
        out.reserve(static_cast<size_t>(dim));

        for (size_t t = 0; t < spec.obs_terms.size(); ++t) {
            const ObsTerm& term = spec.obs_terms[t];
            if (!build_term(term, input, frame, error))
                return false;

            const size_t history_length = static_cast<size_t>(term.history);
            std::deque<std::vector<float>>& history = term_history[t];
            if (!primed) {
                // Prime every slot so the first observation already has a full window.
                history.clear();
                for (size_t i = 0; i < history_length; ++i)
                    history.push_back(frame);
            } else {
                history.push_back(frame);
                while (history.size() > history_length)
                    history.pop_front();
            }
        }
        primed = true;

        for (const auto& history : term_history)
            for (const auto& entry : history)
                out.insert(out.end(), entry.begin(), entry.end());

        if (static_cast<int>(out.size()) != dim) {
            error = "observation assembly produced an unexpected size";
            return false;
        }

        if (!spec.normalization_mean.empty()) {
            if (out.size() != spec.normalization_mean.size()) {
                error = "observation size does not match the spec's normalization length";
                return false;
            }
            for (size_t i = 0; i < out.size(); ++i) {
                const float stddev = spec.normalization_std[i];
                out[i] = stddev > 0.0f ? (out[i] - spec.normalization_mean[i]) / stddev : out[i];
            }
        }
        return true;
    }

} // namespace bud::rl
