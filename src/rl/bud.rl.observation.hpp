#pragma once

// Assembles a policy observation vector from a robot state snapshot, following a PolicySpec.
// Everything here is in the policy's joint order: the caller maps asset joints to policy joints
// once, so this stays packing/scaling plus the per-term history buffers.

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "src/rl/bud.rl.policy_spec.hpp"

namespace bud::rl {

    struct ObservationInput {
        // All arrays are in policy joint order and have joint_count entries.
        const float* joint_pos = nullptr;
        const float* joint_vel = nullptr;
        const float* joint_torque = nullptr;
        const float* last_action = nullptr;
        const float* default_pose = nullptr;
        size_t joint_count = 0;

        // Base quantities in the robot (URDF) body frame: what external policies are trained on.
        glm::vec3 base_ang_vel{ 0.0f };
        glm::vec3 projected_gravity{ 0.0f };
        glm::vec3 base_lin_vel{ 0.0f };
        // The same quantities in the engine frame, for specs that ask for frame "engine".
        glm::vec3 base_ang_vel_engine{ 0.0f };
        glm::vec3 projected_gravity_engine{ 0.0f };
        glm::vec3 base_lin_vel_engine{ 0.0f };
        glm::vec3 command{ 0.0f };
        // Seconds since the last reset, for Phase terms (each term applies its own period).
        float policy_time = 0.0f;
    };

    // Fills out with one term's raw values (no scaling).
    bool build_term(const ObsTerm& term, const ObservationInput& input, std::vector<float>& out,
                    std::string& error);

    // Maintains per-term history and produces the flattened observation. Layout matches the
    // reference deployment: for each term in config order, its frames from oldest to newest.
    class ObservationBuilder {
    public:
        bool initialize(const PolicySpec& spec, size_t joint_count, std::string& error);
        // Pushes a new frame and writes the flattened observation.
        bool build(const PolicySpec& spec, const ObservationInput& input, std::vector<float>& out,
                   std::string& error);
        // Marks the buffers empty so the next build primes every history slot with that frame, which
        // is what the reference reset does; the policy then always sees a full history window.
        void reset();
        int observation_dim() const { return dim; }

    private:
        std::vector<std::deque<std::vector<float>>> term_history;
        std::vector<float> frame;
        int dim = 0;
        bool primed = false;
    };

} // namespace bud::rl
