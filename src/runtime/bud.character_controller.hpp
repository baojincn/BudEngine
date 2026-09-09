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

        // Place the capsule's FEET on the highest static surface under the spawn XZ.
        // Spawning from a fixed camera height embeds the capsule bottom into thin
        // slabs/steps whenever the capsule dimensions change (Jolt then blocks every
        // movement cast -> "WASD dead"). Call at load, before unstick_from.
        void snap_to_ground(const std::vector<bud::math::vec3>& body_positions,
                            const std::vector<bud::math::vec3>& body_half_extents);

        // If the capsule currently overlaps any of the given AABBs, lift it straight up
        // until it is free and teleport there. Returns the overlap count at the original
        // spot: 0 = spawn was already free, negative = still stuck after rising max_rise.
        // Guards against starting a run welded inside a static volume, from which a
        // CharacterVirtual cannot recover (it would be unable to move at all).
        int unstick_from(const std::vector<bud::math::vec3>& body_positions,
                         const std::vector<bud::math::vec3>& body_half_extents,
                         float max_rise = 8.0f);

        bud::physics::PhysicsScene* get_physics_scene() const { return physics_scene; }

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
    };

} // namespace bud::scene