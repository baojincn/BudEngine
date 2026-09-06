#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <optional>

#include "src/core/bud.math.hpp"

namespace bud::physics {

    // Jolt uses meters internally. Bud uses meters (1.0f = 1 m) as defined in
    // bud.core.hpp. No scaling needed.

    enum class MotionType : uint8_t {
        Static,
        Kinematic,
        Dynamic
    };

    enum class ShapeType : uint8_t {
        Sphere,
        Box,
        Capsule,
        ConvexHull,
        Mesh,
        Compound
    };

    struct PhysicsMaterial {
        float friction = 0.5f;
        float restitution = 0.3f;
    };

    struct ShapeDesc {
        ShapeType type = ShapeType::Box;

        bud::math::vec3 half_extent{0.5f, 0.5f, 0.5f};

        float radius = 0.5f;

        float capsule_radius = 0.3f;
        float capsule_half_height = 0.7f;

        std::vector<bud::math::vec3> vertices;
        std::vector<uint32_t> indices;
    };

    struct RigidBodyDesc {
        bud::math::vec3 position{0.0f};
        bud::math::quaternion rotation{1.0f, 0.0f, 0.0f, 0.0f};
        MotionType motion_type = MotionType::Static;
        float mass = 1.0f;
        bool allow_sleep = true;
        bool is_sensor = false;
        bool is_ccd = false;
        PhysicsMaterial material;
        ShapeDesc shape;
    };

    struct SoftBodyDesc {
        std::vector<bud::math::vec3> vertices;
        std::vector<uint32_t> indices;
        float mass = 1.0f;
        bool is_skinned = false;
    };

    struct RaycastResult {
        bool hit = false;
        float fraction = 1.0f;
        bud::math::vec3 hit_point{0.0f};
        bud::math::vec3 hit_normal{0.0f};
        void* user_data = nullptr;
    };

    struct ShapeCastResult {
        bool hit = false;
        float fraction = 1.0f;
        bud::math::vec3 hit_point{0.0f};
        bud::math::vec3 hit_normal{0.0f};
        void* user_data = nullptr;
    };

    struct RigidBodyHandle {
        uint32_t id = ~0u;
        bool is_valid() const { return id != ~0u; }
    };

    struct SoftBodyHandle {
        uint32_t id = ~0u;
        bool is_valid() const { return id != ~0u; }
    };

    struct ConstraintHandle {
        uint32_t id = ~0u;
        bool is_valid() const { return id != ~0u; }
    };

} // namespace bud::physics