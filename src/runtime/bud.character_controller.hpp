#pragma once

#include <vector>

#include "src/core/bud.math.hpp"

namespace JPH {
    class CharacterVirtual;
    class PhysicsSystem;
    class TempAllocator;
    class BroadPhaseLayerFilter;
    class ObjectLayerFilter;
    class BodyFilter;
    class ShapeFilter;
}

namespace bud::physics {
    class PhysicsScene;
}

namespace bud::scene {

    class CharacterController {
    public:
        CharacterController();
        ~CharacterController();

        CharacterController(const CharacterController&) = delete;
        CharacterController& operator=(const CharacterController&) = delete;

        void init(bud::physics::PhysicsScene* physics, const bud::math::vec3& start_pos,
                  float radius = 0.3f, float height = 1.0f);

        void update(float dt);

        void set_velocity(const bud::math::vec3& velocity);
        bud::math::vec3 get_position() const;
        bud::math::vec3 get_eye_position() const;

        bool is_grounded() const;
        void teleport(const bud::math::vec3& pos);

        float get_capsule_radius() const { return capsule_radius; }
        float get_capsule_height() const { return capsule_height; }
        bud::math::vec3 get_linear_velocity() const;

        // Place the capsule's feet flush onto the static ground surface using downward raycast.
        bool snap_to_ground();

        bud::physics::PhysicsScene* get_physics_scene() const { return physics_scene; }

        void add_ignored_body(uint32_t body_id) {
            ignored_bodies.push_back(body_id);
        }

        void clear_ignored_bodies() {
            ignored_bodies.clear();
        }

    private:
        JPH::CharacterVirtual* character = nullptr;
        bud::physics::PhysicsScene* physics_scene = nullptr;
        // Known-good capsule (the walking worked with this): total height 1.6 m.
        // `capsule_height` follows the Jolt convention: cylinder section only.
        float capsule_height = 1.0f;
        float capsule_radius = 0.3f;
        bud::math::vec3 desired_velocity = bud::math::vec3(0.0f);

        // Anti-wedge watchdog state (see update()).
        bud::math::vec3 last_pos{ 0.0f };
        int stuck_frames = 0;
        bud::math::vec3 gravity = bud::math::vec3(0.0f, -9.80665f, 0.0f);
        // Fallback transforms for non-Jolt backends (e.g. MuJoCo robot simulation mode)
        bud::math::vec3 fallback_position{ 0.0f };
        bud::math::vec3 fallback_velocity{ 0.0f };
        std::vector<uint32_t> ignored_bodies;
    };

} // namespace bud::scene