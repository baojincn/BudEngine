#include "src/robots/bud.robot.lowcmd.hpp"

#include <algorithm>
#include <cmath>

namespace bud::robots {

    const std::array<const char*, G1_NUM_MOTORS> k_g1_joint_names = {
        "left_hip_pitch_joint",
        "left_hip_roll_joint",
        "left_hip_yaw_joint",
        "left_knee_joint",
        "left_ankle_pitch_joint",
        "left_ankle_roll_joint",
        "right_hip_pitch_joint",
        "right_hip_roll_joint",
        "right_hip_yaw_joint",
        "right_knee_joint",
        "right_ankle_pitch_joint",
        "right_ankle_roll_joint",
        "waist_yaw_joint",
        "waist_roll_joint",
        "waist_pitch_joint",
        "left_shoulder_pitch_joint",
        "left_shoulder_roll_joint",
        "left_shoulder_yaw_joint",
        "left_elbow_joint",
        "left_wrist_roll_joint",
        "left_wrist_pitch_joint",
        "left_wrist_yaw_joint",
        "right_shoulder_pitch_joint",
        "right_shoulder_roll_joint",
        "right_shoulder_yaw_joint",
        "right_elbow_joint",
        "right_wrist_roll_joint",
        "right_wrist_pitch_joint",
        "right_wrist_yaw_joint"
    };

    namespace {
        constexpr float k_gain_knee_kp = 300.0f;
        constexpr float k_gain_knee_kd = 4.0f;
        constexpr float k_gain_hip_kp = 150.0f;
        constexpr float k_gain_hip_kd = 3.0f;
        constexpr float k_gain_ankle_kp = 100.0f;
        constexpr float k_gain_ankle_kd = 4.0f;
        constexpr float k_gain_waist_kp = 200.0f;
        constexpr float k_gain_waist_kd = 5.0f;
        constexpr float k_gain_shoulder_kp = 80.0f;
        constexpr float k_gain_shoulder_kd = 3.0f;
        constexpr float k_gain_elbow_kp = 40.0f;
        constexpr float k_gain_elbow_kd = 2.0f;
        constexpr float k_gain_wrist_kp = 30.0f;
        constexpr float k_gain_wrist_kd = 1.5f;
        constexpr float k_gain_default_kp = 40.0f;
        constexpr float k_gain_default_kd = 2.0f;

        constexpr float k_stand_hip_pitch = -0.15f;
        constexpr float k_stand_knee = 0.30f;
        constexpr float k_stand_ankle_pitch = -0.15f;
        constexpr float k_stand_arm_shoulder_pitch = 0.2f;
        constexpr float k_stand_arm_shoulder_roll = 0.2f;
        constexpr float k_stand_arm_elbow = 0.6f;

        constexpr double k_pi = 3.14159265358979323846;
    }

    void get_default_g1_gains(std::string_view joint_name, float& out_kp, float& out_kd) {
        if (joint_name.find("knee") != std::string_view::npos) {
            out_kp = k_gain_knee_kp;
            out_kd = k_gain_knee_kd;
            return;
        }
        if (joint_name.find("ankle") != std::string_view::npos) {
            out_kp = k_gain_ankle_kp;
            out_kd = k_gain_ankle_kd;
            return;
        }
        if (joint_name.find("hip") != std::string_view::npos) {
            out_kp = k_gain_hip_kp;
            out_kd = k_gain_hip_kd;
            return;
        }
        if (joint_name.find("waist") != std::string_view::npos) {
            out_kp = k_gain_waist_kp;
            out_kd = k_gain_waist_kd;
            return;
        }
        if (joint_name.find("shoulder") != std::string_view::npos) {
            out_kp = k_gain_shoulder_kp;
            out_kd = k_gain_shoulder_kd;
            return;
        }
        if (joint_name.find("elbow") != std::string_view::npos) {
            out_kp = k_gain_elbow_kp;
            out_kd = k_gain_elbow_kd;
            return;
        }
        if (joint_name.find("wrist") != std::string_view::npos) {
            out_kp = k_gain_wrist_kp;
            out_kd = k_gain_wrist_kd;
            return;
        }

        out_kp = k_gain_default_kp;
        out_kd = k_gain_default_kd;
    }

    LowCmd make_g1_standing_cmd() {
        LowCmd cmd{};
        for (size_t i = 0; i < G1_NUM_MOTORS; ++i) {
            const char* name = k_g1_joint_names[i];
            float kp = 0.0f;
            float kd = 0.0f;
            get_default_g1_gains(name, kp, kd);
            cmd.motor_cmd[i].mode = 1;
            cmd.motor_cmd[i].kp = kp;
            cmd.motor_cmd[i].kd = kd;
            cmd.motor_cmd[i].dq = 0.0f;
            cmd.motor_cmd[i].tau = 0.0f;
            cmd.motor_cmd[i].q = 0.0f;
        }

        auto set_angle = [&](std::string_view name, float angle) {
            for (size_t i = 0; i < G1_NUM_MOTORS; ++i) {
                if (k_g1_joint_names[i] == name) {
                    cmd.motor_cmd[i].q = angle;
                    break;
                }
            }
        };

        set_angle("left_hip_pitch_joint", k_stand_hip_pitch);
        set_angle("left_knee_joint", k_stand_knee);
        set_angle("left_ankle_pitch_joint", k_stand_ankle_pitch);

        set_angle("right_hip_pitch_joint", k_stand_hip_pitch);
        set_angle("right_knee_joint", k_stand_knee);
        set_angle("right_ankle_pitch_joint", k_stand_ankle_pitch);

        set_angle("left_shoulder_pitch_joint", k_stand_arm_shoulder_pitch);
        set_angle("left_shoulder_roll_joint", k_stand_arm_shoulder_roll);
        set_angle("left_elbow_joint", k_stand_arm_elbow);

        set_angle("right_shoulder_pitch_joint", k_stand_arm_shoulder_pitch);
        set_angle("right_shoulder_roll_joint", -k_stand_arm_shoulder_roll);
        set_angle("right_elbow_joint", k_stand_arm_elbow);

        return cmd;
    }

    std::unordered_map<std::string, float> get_g1_standing_joint_angles() {
        std::unordered_map<std::string, float> angles;
        angles["left_hip_pitch_joint"] = k_stand_hip_pitch;
        angles["left_knee_joint"] = k_stand_knee;
        angles["left_ankle_pitch_joint"] = k_stand_ankle_pitch;

        angles["right_hip_pitch_joint"] = k_stand_hip_pitch;
        angles["right_knee_joint"] = k_stand_knee;
        angles["right_ankle_pitch_joint"] = k_stand_ankle_pitch;

        angles["left_shoulder_pitch_joint"] = k_stand_arm_shoulder_pitch;
        angles["left_shoulder_roll_joint"] = k_stand_arm_shoulder_roll;
        angles["left_elbow_joint"] = k_stand_arm_elbow;

        angles["right_shoulder_pitch_joint"] = k_stand_arm_shoulder_pitch;
        angles["right_shoulder_roll_joint"] = -k_stand_arm_shoulder_roll;
        angles["right_elbow_joint"] = k_stand_arm_elbow;
        return angles;
    }

    void compute_body_orientation(const bud::math::quaternion& eng_rot,
                                  float& out_pitch_rad,
                                  float& out_roll_rad,
                                  float& out_tilt_deg) {
        // In BudEngine coordinates: +Y is Up, +X is Forward, +Z is Right.
        // Transforming local up (0, 1, 0) by orientation quaternion:
        const float w = eng_rot.w;
        const float x = eng_rot.x;
        const float y = eng_rot.y;
        const float z = eng_rot.z;

        const float u_x = 2.0f * (x * y - w * z);
        const float u_y = 1.0f - 2.0f * (x * x + z * z);
        const float u_z = 2.0f * (y * z + w * x);

        out_pitch_rad = std::asin(std::clamp(u_x, -1.0f, 1.0f));
        out_roll_rad = std::asin(std::clamp(u_z, -1.0f, 1.0f));
        out_tilt_deg = static_cast<float>(std::acos(std::clamp(u_y, -1.0f, 1.0f)) * 180.0 / k_pi);
    }

    void apply_standing_balance(LowCmd& cmd,
                                float pitch_rad, float pitch_vel,
                                float roll_rad, float roll_vel,
                                float kp_pitch, float kd_pitch,
                                float kp_roll, float kd_roll) {
        constexpr float k_max_delta_pitch = 0.12f;
        const float delta_pitch = std::clamp(kp_pitch * pitch_rad + kd_pitch * pitch_vel,
                                             -k_max_delta_pitch, k_max_delta_pitch);

        constexpr float k_max_delta_roll = 0.05f;
        const float delta_roll = std::clamp(-(kp_roll * roll_rad + kd_roll * roll_vel),
                                            -k_max_delta_roll, k_max_delta_roll);

        for (size_t i = 0; i < G1_NUM_MOTORS; ++i) {
            const std::string_view name(k_g1_joint_names[i]);
            if (name == "left_ankle_pitch_joint" || name == "right_ankle_pitch_joint")
                cmd.motor_cmd[i].q = k_stand_ankle_pitch + delta_pitch;
            else if (name == "left_hip_pitch_joint" || name == "right_hip_pitch_joint")
                cmd.motor_cmd[i].q = k_stand_hip_pitch + 0.5f * delta_pitch;
            else if (name == "left_ankle_roll_joint" || name == "right_ankle_roll_joint")
                cmd.motor_cmd[i].q = delta_roll;
        }
    }

    std::vector<bud::physics::JointCommand> LowCmd::to_joint_commands() const {
        std::vector<bud::physics::JointCommand> list;
        list.reserve(G1_NUM_MOTORS);
        for (size_t i = 0; i < G1_NUM_MOTORS; ++i) {
            bud::physics::JointCommand jc;
            jc.joint_name = k_g1_joint_names[i];
            jc.q = motor_cmd[i].q;
            jc.dq = motor_cmd[i].dq;
            jc.kp = motor_cmd[i].kp;
            jc.kd = motor_cmd[i].kd;
            jc.tau_ff = motor_cmd[i].tau;
            list.push_back(std::move(jc));
        }
        return list;
    }

} // namespace bud::robots
