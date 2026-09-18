#include "src/rl/bud.rl.policy_spec.hpp"

#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace bud::rl {

    const char* obs_signal_name(ObsSignal signal) {
        switch (signal) {
            case ObsSignal::BaseAngVel:       return "base_ang_vel";
            case ObsSignal::ProjectedGravity: return "projected_gravity";
            case ObsSignal::BaseLinVel:       return "base_lin_vel";
            case ObsSignal::Command:          return "command";
            case ObsSignal::JointPosRel:      return "joint_pos_rel";
            case ObsSignal::JointPos:         return "joint_pos";
            case ObsSignal::JointVel:         return "joint_vel";
            case ObsSignal::JointTorque:      return "joint_torque";
            case ObsSignal::LastAction:       return "last_action";
            case ObsSignal::Phase:            return "phase";
        }
        return "unknown";
    }

    std::optional<ObsSignal> obs_signal_from_name(const std::string& name) {
        if (name == "base_ang_vel")       return ObsSignal::BaseAngVel;
        if (name == "projected_gravity")  return ObsSignal::ProjectedGravity;
        if (name == "base_lin_vel")       return ObsSignal::BaseLinVel;
        if (name == "command")            return ObsSignal::Command;
        if (name == "joint_pos_rel")      return ObsSignal::JointPosRel;
        if (name == "joint_pos")          return ObsSignal::JointPos;
        if (name == "joint_vel")          return ObsSignal::JointVel;
        if (name == "joint_torque")       return ObsSignal::JointTorque;
        if (name == "last_action")        return ObsSignal::LastAction;
        if (name == "phase")              return ObsSignal::Phase;
        return std::nullopt;
    }

    int obs_signal_dim(ObsSignal signal, size_t joint_count) {
        switch (signal) {
            case ObsSignal::BaseAngVel:
            case ObsSignal::ProjectedGravity:
            case ObsSignal::BaseLinVel:
            case ObsSignal::Command:
                return 3;
            case ObsSignal::JointPosRel:
            case ObsSignal::JointPos:
            case ObsSignal::JointVel:
            case ObsSignal::JointTorque:
            case ObsSignal::LastAction:
                return static_cast<int>(joint_count);
            case ObsSignal::Phase:
                return 2;
        }
        return 0;
    }

    int PolicySpec::observation_dim(size_t joint_count) const {
        int dim = 0;
        for (const ObsTerm& term : obs_terms)
            dim += obs_signal_dim(term.signal, joint_count) * term.history;
        return dim;
    }

    namespace {
        std::vector<float> read_float_array(const nlohmann::json& value, size_t expected, bool& ok) {
            ok = false;
            if (!value.is_array())
                return {};
            if (expected != 0 && value.size() != expected)
                return {};
            std::vector<float> out;
            out.reserve(value.size());
            for (const auto& entry : value) {
                if (!entry.is_number())
                    return {};
                out.push_back(entry.get<float>());
            }
            ok = true;
            return out;
        }
    } // namespace

    std::optional<PolicySpec> PolicySpec::load_json(const std::string& path, std::string& error) {
        std::ifstream in(path);
        if (!in) {
            error = "cannot open policy spec '" + path + "'";
            return std::nullopt;
        }

        nlohmann::json root;
        try {
            in >> root;
        } catch (const std::exception& e) {
            error = std::string("policy spec is not valid JSON: ") + e.what();
            return std::nullopt;
        }

        PolicySpec spec;
        spec.name = root.value("name", std::string("policy"));
        spec.policy_dt = root.value("policy_dt", 0.02f);

        const auto obs_it = root.find("obs");
        if (obs_it == root.end() || !obs_it->contains("terms") || !(*obs_it)["terms"].is_array()) {
            error = "policy spec needs obs.terms[]";
            return std::nullopt;
        }
        for (const auto& term_json : (*obs_it)["terms"]) {
            const std::string signal_name = term_json.value("signal", std::string());
            const std::optional<ObsSignal> signal = obs_signal_from_name(signal_name);
            if (!signal) {
                error = "unknown observation signal '" + signal_name + "'";
                return std::nullopt;
            }
            ObsTerm term;
            term.signal = *signal;
            term.history = term_json.value("history", 1);
            term.period = term_json.value("period", 0.8f);
            const std::string frame_name = term_json.value("frame", std::string("robot"));
            if (frame_name == "engine")
                term.frame = ObsFrame::Engine;
            else if (frame_name == "robot")
                term.frame = ObsFrame::Robot;
            else {
                error = "obs.terms[].frame must be 'robot' or 'engine'";
                return std::nullopt;
            }
            if (term_json.contains("scale")) {
                const nlohmann::json& scale = term_json["scale"];
                if (scale.is_number()) {
                    term.scale.push_back(scale.get<float>());
                } else if (scale.is_array()) {
                    bool ok = false;
                    term.scale = read_float_array(scale, 0, ok);
                    if (!ok) {
                        error = "obs.terms[].scale must be a number or an array of numbers";
                        return std::nullopt;
                    }
                } else if (!scale.is_null()) {
                    error = "obs.terms[].scale must be a number or an array";
                    return std::nullopt;
                }
            }
            spec.obs_terms.push_back(std::move(term));
        }

        if (obs_it->contains("normalization") && !(*obs_it)["normalization"].is_null()) {
            const nlohmann::json& norm = (*obs_it)["normalization"];
            if (!norm.contains("mean") || !norm.contains("std")) {
                error = "obs.normalization needs mean and std";
                return std::nullopt;
            }
            bool mean_ok = false;
            bool std_ok = false;
            spec.normalization_mean = read_float_array(norm["mean"], 0, mean_ok);
            spec.normalization_std = read_float_array(norm["std"], 0, std_ok);
            if (!mean_ok || !std_ok || spec.normalization_mean.size() != spec.normalization_std.size()) {
                error = "obs.normalization mean/std must be equal-length number arrays";
                return std::nullopt;
            }
        }

        const auto action_it = root.find("action");
        if (action_it == root.end()) {
            error = "policy spec needs an action block";
            return std::nullopt;
        }
        if (action_it->contains("joint_order")) {
            if (!(*action_it)["joint_order"].is_array()) {
                error = "action.joint_order must be an array of joint names";
                return std::nullopt;
            }
            for (const auto& joint : (*action_it)["joint_order"]) {
                if (!joint.is_string()) {
                    error = "action.joint_order must contain strings";
                    return std::nullopt;
                }
                spec.joint_order.push_back(joint.get<std::string>());
            }
        }
        if (action_it->contains("default_pose")) {
            const nlohmann::json& pose = (*action_it)["default_pose"];
            if (pose.is_string()) {
                spec.default_pose = pose.get<std::string>();
            } else if (pose.is_array()) {
                bool ok = false;
                spec.default_pose_values = read_float_array(pose, 0, ok);
                if (!ok) {
                    error = "action.default_pose must be a pose name or a number array";
                    return std::nullopt;
                }
            } else {
                error = "action.default_pose must be a pose name or a number array";
                return std::nullopt;
            }
        }
        spec.action_scale = action_it->value("scale", 0.25f);
        if (action_it->contains("kp")) {
            bool ok = false;
            spec.action_kp = read_float_array((*action_it)["kp"], 0, ok);
            if (!ok) {
                error = "action.kp must be a number array";
                return std::nullopt;
            }
        }
        if (action_it->contains("kd")) {
            bool ok = false;
            spec.action_kd = read_float_array((*action_it)["kd"], 0, ok);
            if (!ok) {
                error = "action.kd must be a number array";
                return std::nullopt;
            }
        }
        if (action_it->contains("clip")) {
            bool ok = false;
            const std::vector<float> clip = read_float_array((*action_it)["clip"], 2, ok);
            if (!ok) {
                error = "action.clip must be [min, max]";
                return std::nullopt;
            }
            spec.action_clip_min = clip[0];
            spec.action_clip_max = clip[1];
        }

        if (root.contains("inputs")) {
            spec.command_from_joystick = root["inputs"].value("joystick", true);
        }

        if (root.contains("freeze_other_joints") && !root["freeze_other_joints"].is_null()) {
            const nlohmann::json& freeze = root["freeze_other_joints"];
            spec.freeze_other_kp = freeze.value("kp", spec.freeze_other_kp);
            spec.freeze_other_kd = freeze.value("kd", spec.freeze_other_kd);
        }

        if (root.contains("network") && !root["network"].is_null()) {
            const nlohmann::json& network = root["network"];
            spec.network.type = network.value("type", std::string());
            spec.network.weights = network.value("weights", std::string());
            spec.network.input_size = network.value("input_size", 0);
            spec.network.hidden_size = network.value("hidden_size", 0);
            spec.network.actor_hidden = network.value("actor_hidden", 0);
            spec.network.output_size = network.value("output_size", 0);
            spec.network.layers = network.value("layers", 1);
            if (network.contains("layer_sizes"))
                spec.network.layer_sizes = network["layer_sizes"].get<std::vector<int>>();
            if (network.contains("hidden_sizes"))
                spec.network.hidden_sizes = network["hidden_sizes"].get<std::vector<int>>();
            if (spec.network.type != "lstm_mlp" && spec.network.type != "mlp") {
                error = "network.type must be 'mlp' or 'lstm_mlp'";
                return std::nullopt;
            }
            if (spec.network.weights.empty()) {
                error = "network.weights is required";
                return std::nullopt;
            }
        }

        return spec;
    }

    bool PolicySpec::validate(size_t joint_count, std::string& error) const {
        if (policy_dt <= 0.0f || policy_dt > 0.5f) {
            error = "policy_dt must be in (0, 0.5]";
            return false;
        }
        if (obs_terms.empty()) {
            error = "obs.terms is empty";
            return false;
        }
        for (const ObsTerm& term : obs_terms) {
            if (term.history < 1) {
                error = std::string("observation history must be at least 1 (signal '") +
                        obs_signal_name(term.signal) + "')";
                return false;
            }
            if (term.signal == ObsSignal::Phase && term.period <= 0.0f) {
                error = "the phase signal needs a positive period";
                return false;
            }
            const int dim = obs_signal_dim(term.signal, joint_count);
            if (!term.scale.empty() && term.scale.size() != 1 &&
                static_cast<int>(term.scale.size()) != dim) {
                error = std::string("scale for '") + obs_signal_name(term.signal) +
                        "' must have 1 entry or one per component";
                return false;
            }
        }
        if (joint_order.empty()) {
            error = "action.joint_order is empty";
            return false;
        }
        std::unordered_set<std::string> seen;
        for (const std::string& joint : joint_order) {
            if (!seen.insert(joint).second) {
                error = "action.joint_order lists '" + joint + "' more than once";
                return false;
            }
        }
        if (!default_pose_values.empty() && default_pose_values.size() != joint_order.size()) {
            error = "action.default_pose array must match joint_order length";
            return false;
        }
        if (!action_kp.empty() && action_kp.size() != joint_order.size()) {
            error = "action.kp must have one entry per policy joint";
            return false;
        }
        if (!action_kd.empty() && action_kd.size() != joint_order.size()) {
            error = "action.kd must have one entry per policy joint";
            return false;
        }
        if (!normalization_mean.empty()) {
            const int dim = observation_dim(joint_count);
            if (static_cast<int>(normalization_mean.size()) != dim) {
                error = "obs.normalization length does not match the observation dimension";
                return false;
            }
        }
        return true;
    }

} // namespace bud::rl
