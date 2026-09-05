#pragma once

#include <cstdint>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <atomic>
#include <algorithm>

#include "src/core/bud.math.hpp"
#include "src/physics/bud.physics.types.hpp"

namespace JPH {
    class PhysicsSystem;
    class BodyInterface;
    class BroadPhaseLayerInterface;
    class ObjectVsBroadPhaseLayerFilter;
    class ObjectLayerPairFilter;
    class TempAllocator;
    class JobSystem;
    class Body;
    class Shape;
}

namespace bud::physics {

    // SoA (Structure of Arrays) physics scene, following the same pattern as
    // graphics::RenderScene for cache efficiency and multi-threaded processing.
    //
    // Jolt PhysicsSystem is the simulation backend; game-side body metadata is
    // stored in SoA columns. Sync happens at step boundaries:
    //   before step: game-side SoA → Jolt bodies
    //   after step:  Jolt bodies → game-side SoA (for dynamic bodies)
    class PhysicsScene {
    public:
        PhysicsScene();
        ~PhysicsScene();

        PhysicsScene(const PhysicsScene&) = delete;
        PhysicsScene& operator=(const PhysicsScene&) = delete;
        PhysicsScene(PhysicsScene&&) = delete;
        PhysicsScene& operator=(PhysicsScene&&) = delete;

        void init(uint32_t max_bodies = 65536,
                  uint32_t max_body_pairs = 65536,
                  uint32_t max_contact_constraints = 10240);

        void step(float delta_time,
                  int collision_steps = 1,
                  int integration_steps = 1);

        // ------------------------------------------------------------------
        // DoD: SoA columns for rigid body metadata
        // ------------------------------------------------------------------
        std::vector<bud::math::vec3>       body_positions;
        std::vector<bud::math::quaternion> body_rotations;
        std::vector<bud::math::vec3>       body_linear_velocities;
        std::vector<bud::math::vec3>       body_angular_velocities;
        std::vector<float>                 body_masses;
        std::vector<void*>                 body_user_data;

        enum BodyFlags : uint8_t {
            BODY_FLAG_NONE        = 0,
            BODY_FLAG_STATIC      = 1 << 0,
            BODY_FLAG_KINEMATIC   = 1 << 1,
            BODY_FLAG_SENSOR      = 1 << 2,
            BODY_FLAG_CCD         = 1 << 3,
            BODY_FLAG_ALLOW_SLEEP = 1 << 4,
        };
        std::vector<uint8_t> body_flags;

        // Soft body SoA columns (placeholder)
        std::vector<bud::math::vec3> soft_body_positions;
        std::vector<float>           soft_body_masses;
        std::vector<uint8_t>         soft_body_flags;

        std::atomic<size_t> body_count{0};
        std::atomic<size_t> dropped_bodies{0};
        std::atomic<size_t> soft_body_count{0};

        void reset(size_t capacity) {
            body_positions.assign(capacity, bud::math::vec3(0.0f));
            body_rotations.assign(capacity, bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f));
            body_linear_velocities.assign(capacity, bud::math::vec3(0.0f));
            body_angular_velocities.assign(capacity, bud::math::vec3(0.0f));
            body_masses.assign(capacity, 0.0f);
            body_user_data.assign(capacity, nullptr);
            body_flags.assign(capacity, 0);
            body_count.store(0);
            dropped_bodies.store(0);
        }

        inline size_t size() const {
            const size_t count = body_count.load(std::memory_order_relaxed);
            return std::min(count, body_positions.size());
        }

        inline RigidBodyHandle add_rigid_body(const RigidBodyDesc& desc) {
            size_t idx = body_count.fetch_add(1, std::memory_order_relaxed);
            if (idx >= body_positions.size()) [[unlikely]] {
                dropped_bodies.fetch_add(1, std::memory_order_relaxed);
                return {};
            }

            body_positions[idx] = desc.position;
            body_rotations[idx] = desc.rotation;
            body_linear_velocities[idx] = bud::math::vec3(0.0f);
            body_angular_velocities[idx] = bud::math::vec3(0.0f);
            body_masses[idx] = desc.mass;
            body_user_data[idx] = nullptr;

            body_flags[idx] = static_cast<uint8_t>(
                (desc.motion_type == MotionType::Static    ? BODY_FLAG_STATIC    : 0) |
                (desc.motion_type == MotionType::Kinematic ? BODY_FLAG_KINEMATIC : 0) |
                (desc.is_sensor    ? BODY_FLAG_SENSOR      : 0) |
                (desc.is_ccd       ? BODY_FLAG_CCD         : 0) |
                (desc.allow_sleep  ? BODY_FLAG_ALLOW_SLEEP : 0));

            return {static_cast<uint32_t>(idx)};
        }

        // ------------------------------------------------------------------
        // Jolt body sync (called at step boundaries)
        // ------------------------------------------------------------------
        void sync_to_jolt();
        void sync_from_jolt();

        void remove_rigid_body(RigidBodyHandle handle);

        inline void set_body_position(RigidBodyHandle handle, const bud::math::vec3& pos) {
            if (handle.is_valid() && handle.id < body_positions.size())
                body_positions[handle.id] = pos;
        }
        inline void set_body_rotation(RigidBodyHandle handle, const bud::math::quaternion& rot) {
            if (handle.is_valid() && handle.id < body_rotations.size())
                body_rotations[handle.id] = rot;
        }
        inline void set_body_linear_velocity(RigidBodyHandle handle, const bud::math::vec3& vel) {
            if (handle.is_valid() && handle.id < body_linear_velocities.size())
                body_linear_velocities[handle.id] = vel;
        }
        inline void set_body_angular_velocity(RigidBodyHandle handle, const bud::math::vec3& vel) {
            if (handle.is_valid() && handle.id < body_angular_velocities.size())
                body_angular_velocities[handle.id] = vel;
        }

        inline bud::math::vec3 get_body_position(RigidBodyHandle handle) const {
            if (handle.is_valid() && handle.id < body_positions.size())
                return body_positions[handle.id];
            return {};
        }
        inline bud::math::quaternion get_body_rotation(RigidBodyHandle handle) const {
            if (handle.is_valid() && handle.id < body_rotations.size())
                return body_rotations[handle.id];
            return {};
        }
        inline bud::math::vec3 get_body_linear_velocity(RigidBodyHandle handle) const {
            if (handle.is_valid() && handle.id < body_linear_velocities.size())
                return body_linear_velocities[handle.id];
            return {};
        }
        inline bud::math::vec3 get_body_angular_velocity(RigidBodyHandle handle) const {
            if (handle.is_valid() && handle.id < body_angular_velocities.size())
                return body_angular_velocities[handle.id];
            return {};
        }

        void apply_force(RigidBodyHandle handle, const bud::math::vec3& force);
        void apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse);

        // ------------------------------------------------------------------
        // Soft body (placeholder)
        // ------------------------------------------------------------------
        SoftBodyHandle create_soft_body(const SoftBodyDesc& desc);
        void           remove_soft_body(SoftBodyHandle handle);

        // ------------------------------------------------------------------
        // Spatial queries
        // ------------------------------------------------------------------
        std::optional<RaycastResult>   raycast(const bud::math::vec3& from, const bud::math::vec3& to) const;
        std::optional<ShapeCastResult> sphere_cast(const bud::math::vec3& from, const bud::math::vec3& to, float radius) const;

        // ------------------------------------------------------------------
        // Contact callbacks
        // ------------------------------------------------------------------
        using ContactCallback = std::function<void(void* body_a_user_data,
                                                    void* body_b_user_data,
                                                    const bud::math::vec3& contact_point,
                                                    const bud::math::vec3& contact_normal,
                                                    float penetration_depth)>;

        void set_contact_begin_callback(ContactCallback cb);
        void set_contact_persist_callback(ContactCallback cb);
        void set_contact_end_callback(ContactCallback cb);

        void set_gravity(const bud::math::vec3& gravity);
        bud::math::vec3 get_gravity() const;

        uint32_t get_active_body_count() const;
        uint32_t get_total_body_count() const;

    private:
        void create_jolt_bodies();

        std::unique_ptr<JPH::PhysicsSystem> physics_system;

        std::unique_ptr<JPH::BroadPhaseLayerInterface>  broad_phase_layer_interface;
        std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> object_vs_broadphase_filter;
        std::unique_ptr<JPH::ObjectLayerPairFilter>     object_layer_pair_filter;

        std::unique_ptr<JPH::TempAllocator> temp_allocator;
        std::unique_ptr<JPH::JobSystem>     job_system;

        // Jolt body IDs, parallel to SoA columns
        std::vector<uint32_t> jolt_body_ids;
        // Jolt shapes the body owns (ref-counted)
        std::vector<JPH::Shape*> jolt_shapes;

        std::vector<ShapeDesc> pending_shapes;

        ContactCallback contact_begin_cb;
        ContactCallback contact_persist_cb;
        ContactCallback contact_end_cb;

        bool initialized = false;
    };

} // namespace bud::physics