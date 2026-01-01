module;

#include <cmath>
#include <array>
#include <concepts>

export module bud.math;

import bud.core;

export namespace bud::math {
    // 3D Vector
    struct float3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        float3() = default;
        float3(float x, float y, float z) : x(x), y(y), z(z) {}

        float3 operator+(const float3& other) const {
            return { x + other.x, y + other.y, z + other.z };
        }

        float3 operator-(const float3& other) const {
            return { x - other.x, y - other.y, z - other.z };
        }

        float3 operator*(float scalar) const {
            return { x * scalar, y * scalar, z * scalar };
        }
    };

    // 4D Vector
    struct float4 {
        float x, y, z, w;
        float4() = default;
        float4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    };

    // 4x4 Matrix
    struct mat4 {
        std::array<float, 16> data{};
        mat4() = default;
    };
}
