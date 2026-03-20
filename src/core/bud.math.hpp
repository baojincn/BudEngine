#pragma once

// Force GLM to use Radians and Vulkan Depth Range (0..1)
// crucial for consistent matrices across compiled units
#ifndef GLM_FORCE_RADIANS
#define GLM_FORCE_RADIANS
#endif
#ifndef GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#endif

// 集中管理 GLM 依赖
#include <cmath>
#include <algorithm>
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


	inline float distance2(const vec3& a, const vec3& b) {
		vec3 diff = a - b;
		return glm::dot(diff, diff);
	}

	inline mat4 ortho_vk(float left, float right, float bottom, float top, float cam_near, float cam_far) {
		mat4 proj = glm::ortho(left, right, bottom, top, cam_near, cam_far);
		proj[1][1] *= -1;

		return proj;
	}

	inline mat4 ortho_vk_reversed(float left, float right, float bottom, float top, float cam_near, float cam_far) {
		mat4 proj = glm::ortho(left, right, bottom, top, cam_far, cam_near);
		proj[1][1] *= -1;

		return proj;
	}

	inline mat4 perspective_vk(float fov, float aspect, float near_plane, float far_plane) {
		mat4 proj = glm::perspective(glm::radians(fov), aspect, near_plane, far_plane);
		proj[1][1] *= -1; // Vulkan Y-flip
		return proj;
	}

	inline mat4 perspective_vk_reversed(float fov, float aspect, float near_plane, float far_plane) {
		const float f = 1.0f / std::tan(glm::radians(fov) * 0.5f);
		mat4 proj(0.0f);
		proj[0][0] = f / aspect;
		proj[1][1] = -f; // Vulkan Y-flip
		proj[2][2] = near_plane / (far_plane - near_plane);
		proj[2][3] = -1.0f;
		proj[3][2] = (far_plane * near_plane) / (far_plane - near_plane);
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

		AABB() = default;
		AABB(const vec3& _min, const vec3& _max) : min(_min), max(_max) {}

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

		inline bool intersects(const AABB& other) const {
			return (min.x <= other.max.x && max.x >= other.min.x) &&
				   (min.y <= other.max.y && max.y >= other.min.y) &&
				   (min.z <= other.max.z && max.z >= other.min.z);
		}

		inline bool contains(const vec3& p) const {
			return (p.x >= min.x && p.x <= max.x) &&
				   (p.y >= min.y && p.y <= max.y) &&
				   (p.z >= min.z && p.z <= max.z);
		}

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

	// Morton Code / Z-Order Curve Utilities for (H)LBVH

	// Expands a 10-bit integer into 30 bits
	// by inserting 2 zeros after each bit.
	inline uint32_t expand_bits(uint32_t v) {
		v = (v * 0x00010001u) & 0xFF0000FFu;
		v = (v * 0x00000101u) & 0x0F00F00Fu;
		v = (v * 0x00000011u) & 0xC30C30C3u;
		v = (v * 0x00000005u) & 0x49249249u;
		return v;
	}

	// Calculates a 30-bit Morton code for a 3D point
	// Assumes x, y, and z are normalized to the range [0.0, 1.0]
	inline uint32_t morton_3d(float x, float y, float z) {
		x = std::clamp(x * 1024.0f, 0.0f, 1023.0f);
		y = std::clamp(y * 1024.0f, 0.0f, 1023.0f);
		z = std::clamp(z * 1024.0f, 0.0f, 1023.0f);
		uint32_t xx = expand_bits(static_cast<uint32_t>(x));
		uint32_t yy = expand_bits(static_cast<uint32_t>(y));
		uint32_t zz = expand_bits(static_cast<uint32_t>(z));
		return (xx << 2) | (yy << 1) | zz;
	}

	// Calculates a 30-bit Morton code for a point inside a given AABB
	inline uint32_t compute_morton_code(const vec3& point, const AABB& global_bounds) {
		vec3 size = global_bounds.size();
		// Avoid division by zero
		if (size.x == 0.0f) size.x = 0.0001f;
		if (size.y == 0.0f) size.y = 0.0001f;
		if (size.z == 0.0f) size.z = 0.0001f;

		vec3 centroid_normalized = (point - global_bounds.min) / size;
		return morton_3d(centroid_normalized.x, centroid_normalized.y, centroid_normalized.z);
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
