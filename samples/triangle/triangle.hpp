#pragma once

#include <memory>
#include <atomic>
#include <string>
#include "src/runtime/bud.game.hpp"
#include "src/streaming/bud.streaming.manager.hpp"
#include "src/robots/bud.robot.avatar.hpp"

class TriangleApp : public bud::game::GameFramework {
private:
	std::shared_ptr<std::atomic<int>> pending_mesh_loads = std::make_shared<std::atomic<int>>(1);
	std::unique_ptr<bud::robots::RobotAvatarController> m_robot_avatar;
	float m_test_duration = 0.0f;
	float m_elapsed_time = 0.0f;
	float m_last_log_time = 0.0f;
	float m_min_pelvis_y = 100.0f;
	float m_max_pelvis_y = -100.0f;
	float m_max_tilt_deg = 0.0f;
	bool m_test_failed = false;
	std::string m_policy_path;
	std::string m_policy_spec_path;
	bud::math::vec3 m_policy_command{ 0.0f };
	bool m_has_policy_command = false;

public:
	void set_test_duration(float duration) {
		m_test_duration = duration;
	}

	// Optional external ONNX locomotion policy. An empty onnx path with a spec loads the null policy
	// (holds the default pose), which exercises the observation/action plumbing without a model.
	void set_policy(const std::string& onnx_path, const std::string& spec_path) {
		m_policy_path = onnx_path;
		m_policy_spec_path = spec_path;
	}

	void set_policy_command(const bud::math::vec3& command) {
		m_policy_command = command;
		m_has_policy_command = true;
	}

	bool has_test_failed() const {
		return m_test_failed;
	}

	bool is_fully_loaded() const override;

	void on_init(const bud::game::AppConfig& config) override;

	void on_update(float delta_time) override;

	void on_shutdown() override;
};
