module;

// 集中管理 GLM 依赖
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module bud.math;

export namespace bud::math {
	// 导出 GLM 类型，方便其他模块使用
	using glm::vec2;
	using glm::vec3;
	using glm::vec4;
	using glm::mat4;
	using glm::radians;
	using glm::lookAt;
	using glm::perspective;
	using glm::normalize;
	using glm::cross;
	using glm::inverse;

	// 常量定义
	constexpr float YAW = -90.0f;
	constexpr float PITCH = 0.0f;
	constexpr float SPEED = 1.0f;
	constexpr float SENSITIVITY = 0.1f;
	constexpr float ZOOM = 45.0f;

	// Vulkan 特供投影矩阵 (处理 Y 轴翻转)
	mat4 perspective_vk(float fov, float aspect, float near_plane, float far_plane) {
		mat4 proj = glm::perspective(glm::radians(fov), aspect, near_plane, far_plane);
		proj[1][1] *= -1; // Vulkan Y-flip
		return proj;
	}
}
