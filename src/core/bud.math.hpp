#pragma once

// 集中管理 GLM 依赖
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace bud::math {
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


	inline mat4 ortho_vk(float left, float right, float bottom, float top, float near, float far) {
		mat4 proj = glm::ortho(left, right, bottom, top, near, far);
		proj[1][1] *= -1;

		return proj;
	}

	inline mat4 perspective_vk(float fov, float aspect, float near_plane, float far_plane) {
		mat4 proj = glm::perspective(glm::radians(fov), aspect, near_plane, far_plane);
		proj[1][1] *= -1; // Vulkan Y-flip
		return proj;
	}

	// 几何体
	struct BoundingSphere {
		vec3 center{ 0.0f };
		float radius = 0.0f;

		inline BoundingSphere transform(const mat4& m) const {
			vec4 new_center = m * vec4(center, 1.0f);
			// Uniform scale assumption for radius transform
			float max_scale = std::max(std::max(length(vec3(m[0])), length(vec3(m[1]))), length(vec3(m[2])));
			return { vec3(new_center), radius * max_scale };
		}
	};

	struct AABB {
		vec3 min{ std::numeric_limits<float>::max() };
		vec3 max{ std::numeric_limits<float>::lowest() };

		inline void merge(const vec3& p) {
			min = glm::min(min, p);
			max = glm::max(max, p);
		}

		inline void merge(const AABB& other) {
			min = glm::min(min, other.min);
			max = glm::max(max, other.max);
		}

		inline vec3 center() const { return (min + max) * 0.5f; }
		inline vec3 size() const { return max - min; }

		inline AABB transform(const mat4& m) const {
			vec3 corners[8] = {
				{min.x, min.y, min.z}, {min.x, min.y, max.z},
				{min.x, max.y, min.z}, {min.x, max.y, max.z},
				{max.x, min.y, min.z}, {max.x, min.y, max.z},
				{max.x, max.y, min.z}, {max.x, max.y, max.z}
			};

			AABB res;
			for (auto& c : corners) {
				res.merge(vec3(m * vec4(c, 1.0f)));
			}
			return res;
		}
	};

	struct Frustum {
		vec4 planes[6];

		inline void update(const mat4& vp) {
			mat4 m = transpose(vp);
			planes[0] = m[3] + m[0]; // Left
			planes[1] = m[3] - m[0]; // Right
			planes[2] = m[3] + m[1]; // Bottom
			planes[3] = m[3] - m[1]; // Top
			planes[4] = m[3] + m[2]; // Near
			planes[5] = m[3] - m[2]; // Far

			for (auto& p : planes) {
				float len = length(vec3(p));
				p /= len;
			}
		}
	};

	inline bool intersect_sphere_frustum(const BoundingSphere& s, const Frustum& f) {
		for (const auto& plane : f.planes) {
			if (dot(vec3(plane), s.center) + plane.w < -s.radius)
				return false;
		}
		return true;
	}

	inline bool intersect_aabb_frustum(const AABB& b, const Frustum& f) {
		// Optimization: Check if all 8 corners are outside one plane
		for (const auto& plane : f.planes) {
			int out = 0;
			vec3 corners[8] = {
				{b.min.x, b.min.y, b.min.z}, {b.min.x, b.min.y, b.max.z},
				{b.min.x, b.max.y, b.min.z}, {b.min.x, b.max.y, b.max.z},
				{b.max.x, b.min.y, b.min.z}, {b.max.x, b.min.y, b.max.z},
				{b.max.x, b.max.y, b.min.z}, {b.max.x, b.max.y, b.max.z}
			};

			for (const auto& c : corners) {
				if (dot(vec3(plane), c) + plane.w < 0) out++;
			}
			if (out == 8) return false;
		}
		return true;
	}

}

//export inline auto operator*(const bud::math::mat4& a, const bud::math::mat4& b) -> decltype(auto) {
//	return a * b;
//}

inline bud::math::mat4 operator*(const bud::math::mat4& a, const bud::math::mat4& b) {
	using glm::operator*;
	return a * b;
}

// Column vector 
inline bud::math::vec4 operator*(const bud::math::mat4& m, const bud::math::vec4& v) {
	using glm::operator*;
	return m * v;
}

// Row vector
inline bud::math::vec4 operator*(const bud::math::vec4& v, const bud::math::mat4& m) {
	using glm::operator*;
	return v * m;
}

inline bud::math::vec3 operator+(const bud::math::vec3& v1, const bud::math::vec3& v2) {
	using glm::operator+;
	return v1 + v2;
}

inline bud::math::vec3 operator-(const bud::math::vec3& v1, const bud::math::vec3& v2) {
	using glm::operator-;
	return v1 - v2;
}

inline bud::math::vec4 operator+(const bud::math::vec4& v1, const bud::math::vec4& v2) {
	using glm::operator+;
	return v1 + v2;
}

inline bud::math::vec4 operator-(const bud::math::vec4& v1, const bud::math::vec4& v2) {
	using glm::operator-;
	return v1 - v2;
}



inline bud::math::mat4 operator*(const bud::math::mat4& m, float scalar) {
	using glm::operator*;
	return m * scalar;
}

inline bud::math::mat4 operator*(float scalar, const bud::math::mat4& m) {
	using glm::operator*;
	return scalar * m;
}
