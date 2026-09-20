#pragma once

// PhysicsScene is the engine facing facade over a physics backend.
//
// It used to be the Jolt implementation itself. Now it owns a PhysicsWorldBase and forwards, so
// the engine can run a project on Jolt (game worlds) or MuJoCo (robots) without any call site
// knowing which is active. The Jolt accessors at the bottom are transitional: they exist only for
// the character controller and the robot loader, and disappear when those move onto the neutral
// articulation API.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "src/core/bud.math.hpp"
#include "src/physics/bud.physics.types.hpp"
#include "src/physics/bud.physics.world.hpp"

namespace JPH {
    class PhysicsSystem;
    class TempAllocator;
}

namespace bud::physics {

    class PhysicsScene {
    public:
        PhysicsScene();
        ~PhysicsScene();

        PhysicsScene(const PhysicsScene&) = delete;
        PhysicsScene& operator=(const PhysicsScene&) = delete;
        PhysicsScene(PhysicsScene&&) = delete;
        PhysicsScene& operator=(PhysicsScene&&) = delete;

        // Backend selection: Jolt for game worlds, MuJoCo for robot projects. The capacities are
        // ignored by backends that size themselves (MuJoCo does).
        void init(const PhysicsWorldConfig& config);
        void init(uint32_t max_bodies = 65536,
                  uint32_t max_body_pairs = 65536,
                  uint32_t max_contact_constraints = 10240,
                  PhysicsBackend backend = PhysicsBackend::Jolt);

        void step(float delta_time,
                  int collision_steps = 1,
                  int integration_steps = 1);

        // --- rigid bodies ---
        RigidBodyHandle add_rigid_body(const RigidBodyDesc& desc);
        void remove_rigid_body(RigidBodyHandle handle);
        size_t size() const { return body_count; }
        const RigidBodyStateSoA& get_body_states() const;

        // --- spatial queries ---
        std::optional<RaycastResult>   raycast(const bud::math::vec3& from, const bud::math::vec3& to) const;
        std::optional<ShapeCastResult> sphere_cast(const bud::math::vec3& from, const bud::math::vec3& to, float radius) const;

        // --- contacts ---
        void set_contact_begin_callback(ContactCallback callback);
        void set_contact_persist_callback(ContactCallback callback);
        void set_contact_end_callback(ContactCallback callback);

        // --- settings ---
        void set_gravity(const bud::math::vec3& gravity);
        bud::math::vec3 get_gravity() const;
        uint32_t get_active_body_count() const;
        uint32_t get_total_body_count() const;
        float get_ground_plane_height() const {
            if (world)
                return world->get_ground_plane_height();
            else
                return 0.0f;
        }

        // Kept so existing call sites stay unchanged; forwarding to the backend, which mirrors the
        // SoA into its own solver at step boundaries anyway.
        void sync_to_jolt();

        PhysicsWorldBase& get_world() { return *world; }
        const PhysicsWorldBase& get_world() const { return *world; }
        PhysicsBackend get_backend() const { return backend; }

        // ------------------------------------------------------------------
        // Transitional Jolt specific access
        // ------------------------------------------------------------------
        // The character controller uses Jolt's CharacterVirtual and the robot loader still builds a
        // Jolt ragdoll, so both need the raw system. These return nullptr on a non-Jolt backend.
        JPH::PhysicsSystem* get_jolt_system() const;
        JPH::TempAllocator* get_temp_allocator() const;

        // Parallel to the old scene: guards the SoA columns against concurrent readers.
        std::mutex body_mutex;

    private:
        std::unique_ptr<PhysicsWorldBase> world;
        PhysicsBackend backend = PhysicsBackend::Jolt;
        size_t body_count = 0;
    };

} // namespace bud::physics
