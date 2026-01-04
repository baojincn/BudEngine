module;

// 集中管理 GLM 依赖
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module bud.math;


export namespace bud::math {
	// 导出 GLM 类型，方便其他模块使用
	using vec2 = glm::vec2;
	using vec3 = glm::vec3;
	using vec4 = glm::vec4;

	using mat2 = glm::mat2;
	using mat3 = glm::mat3;
	using mat4 = glm::mat4;


	using glm::radians;
	using glm::lookAt;
	using glm::perspective;
	using glm::normalize;
	using glm::cross;
	using glm::inverse;

	using glm::transpose;
	using glm::inverse;
	using glm::determinant;
	using glm::dot;
	using glm::cross;
	using glm::normalize;
	using glm::length;
	using glm::distance;

	using glm::translate;
	using glm::rotate;
	using glm::scale;
	using glm::reflect;
	using glm::refract;

	using glm::translate;
	using glm::rotate;
	using glm::scale;
	using glm::lookAt;
	using glm::perspective;
	using glm::ortho;

	// 常量
	constexpr float PI = 3.14159265358979323846f;

	constexpr float YAW = -90.0f;
	constexpr float PITCH = 0.0f;
	constexpr float SPEED = 1.0f;
	constexpr float SENSITIVITY = 0.1f;
	constexpr float ZOOM = 45.0f;


	mat4 ortho_vk(float left, float right, float bottom, float top, float near, float far) {
		mat4 proj = glm::ortho(left, right, bottom, top, near, far);
		proj[1][1] *= -1;

		return proj;
	}

	mat4 perspective_vk(float fov, float aspect, float near_plane, float far_plane) {
		mat4 proj = glm::perspective(glm::radians(fov), aspect, near_plane, far_plane);
		proj[1][1] *= -1; // Vulkan Y-flip
		return proj;
	}

}

//export inline auto operator*(const bud::math::mat4& a, const bud::math::mat4& b) -> decltype(auto) {
//	return a * b;
//}

export inline bud::math::mat4 operator*(const bud::math::mat4& a, const bud::math::mat4& b) {
	using glm::operator*;
	return a * b;
}

// Column vector 
export inline bud::math::vec4 operator*(const bud::math::mat4& m, const bud::math::vec4& v) {
	using glm::operator*;
	return m * v;
}

// Row vector
export inline bud::math::vec4 operator*(const bud::math::vec4& v, const bud::math::mat4& m) {
	using glm::operator*;
	return v * m;
}

export inline bud::math::vec3 operator+(const bud::math::vec3& v1, const bud::math::vec3& v2) {
	using glm::operator+;
	return v1 + v2;
}

export inline bud::math::vec3 operator-(const bud::math::vec3& v1, const bud::math::vec3& v2) {
	using glm::operator-;
	return v1 - v2;
}

export inline bud::math::vec4 operator+(const bud::math::vec4& v1, const bud::math::vec4& v2) {
	using glm::operator+;
	return v1 + v2;
}

export inline bud::math::vec4 operator-(const bud::math::vec4& v1, const bud::math::vec4& v2) {
	using glm::operator-;
	return v1 - v2;
}



export inline bud::math::mat4 operator*(const bud::math::mat4& m, float scalar) {
	using glm::operator*;
	return m * scalar;
}

export inline bud::math::mat4 operator*(float scalar, const bud::math::mat4& m) {
	using glm::operator*;
	return scalar * m;
}
