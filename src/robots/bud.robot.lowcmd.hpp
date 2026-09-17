#pragma once

#include "src/core/bud.math.hpp"
#include "src/physics/bud.physics.world.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bud::robots {

    struct MotorCmd {
        uint8_t mode = 0;    // 0: idle/damping, 1: PD torque
        float q = 0.0f;      // target angle (rad)
        float dq = 0.0f;     // target velocity (rad/s)
        float kp = 0.0f;     // stiffness (N*m/rad)
        float kd = 0.0f;     // damping (N*m/(rad/s))
        float tau = 0.0f;    // feed-forward torque (N*m)
    };

    struct IMUState {
        bud::math::quaternion quaternion{ 1.0f, 0.0f, 0.0f, 0.0f };
        bud::math::vec3 gyroscope{ 0.0f };     // rad/s
        bud::math::vec3 accelerometer{ 0.0f }; // m/s^2
        bud::math::vec3 rpy{ 0.0f };           // roll, pitch, yaw (rad)
    };

    struct MotorState {
        uint8_t mode = 0;
        float q = 0.0f;          // measured angle (rad)
        float dq = 0.0f;         // measured velocity (rad/s)
        float tau_est = 0.0f;    // measured/estimated torque (N*m)
        float temperature = 0.0f;
    };

    inline constexpr size_t G1_NUM_MOTORS = 29;

    // Pelvis height above the standing surface for the official G1 stance (metres). This is a
    // control/stance parameter, not robot geometry, so it lives with the G1 controller instead of
    // being baked into the cooked asset or assumed by the generic physics backend.
    inline constexpr float k_g1_standing_pelvis_height = 0.785f;

    struct LowCmd {
        std::array<MotorCmd, G1_NUM_MOTORS> motor_cmd{};

        // Convert to generic physics JointCommand list
        std::vector<bud::physics::JointCommand> to_joint_commands() const;
    };

    struct LowState {
        IMUState imu;
        std::array<MotorState, G1_NUM_MOTORS> motor_state{};
    };

    // Official G1 29DoF joint names in canonical transmission order
    extern const std::array<const char*, G1_NUM_MOTORS> k_g1_joint_names;

    // Gain lookup for a joint group
    void get_default_g1_gains(std::string_view joint_name, float& out_kp, float& out_kd);

    // Creates the official stable standing pose command
    LowCmd make_g1_standing_cmd();

    // Standing pose joint angles as a map (convenient for spawn pose initialization)
    std::unordered_map<std::string, float> get_g1_standing_joint_angles();

    // Computes pitch and roll angles (in radians) and total tilt (in degrees) from body orientation
    // in BudEngine coordinates (where Y is Up, X is Forward, Z is Right).
    void compute_body_orientation(const bud::math::quaternion& eng_rot,
                                  float& out_pitch_rad,
                                  float& out_roll_rad,
                                  float& out_tilt_deg);

    // Proportional-derivative balance compensation for standing stance.
    // pitch_rad: pelvis pitch in radians (positive = tilted forward)
    // pitch_vel: pelvis pitch angular velocity in rad/s (positive = tilting forward)
    // roll_rad: pelvis roll in radians (positive = tilted right)
    // roll_vel: pelvis roll angular velocity in rad/s (positive = tilting right)
    void apply_standing_balance(LowCmd& cmd,
                                float pitch_rad, float pitch_vel,
                                float roll_rad, float roll_vel,
                                float kp_pitch = 1.0f, float kd_pitch = 0.10f,
                                float kp_roll = 0.5f, float kd_roll = 0.05f);

} // namespace bud::robots
