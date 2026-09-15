#pragma once

// Jolt implementation of PhysicsWorldBase.
//
// Jolt is the game world backend: triangle mesh levels, tens of thousands of rigid bodies and
// already coupled to our cloth and rendering paths. It has no articulation solver (a robot is
// separate rigid bodies joined by constraints) and its joint motors are soft springs, which is
// why the robot work moved to the MuJoCo backend; this one stays responsible for the world.
//
// Bodies live in the neutral SoA state (RigidBodyStateSoA) and are mirrored into Jolt at step
// boundaries:
//   before step: SoA -> Jolt bodies (sync_to_jolt)
//   after step:  Jolt bodies -> SoA  (sync_from_jolt, dynamic bodies only)

#include "src/physics/bud.physics.world.hpp"

#include <atomic>
#include <mutex>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

namespace JPH {
    class PhysicsSystem;
    class BodyInterface;
    class BroadPhaseLayerInterface;
    class ObjectVsBroadPhaseLayerFilter;
    class ObjectLayerPairFilter;
    class TempAllocator;
    class JobSystem;
    class Body;
    class ContactListener;
    class BodyActivationListener;
}

namespace bud::physics {

    class JoltPhysicsWorld final : public PhysicsWorldBase {
    public:
        JoltPhysicsWorld();
        ~JoltPhysicsWorld() override;

        JoltPhysicsWorld(const JoltPhysicsWorld&) = delete;
        JoltPhysicsWorld& operator=(const JoltPhysicsWorld&) = delete;
        JoltPhysicsWorld(JoltPhysicsWorld&&) = delete;
        JoltPhysicsWorld& operator=(JoltPhysicsWorld&&) = delete;

        bool init(const PhysicsWorldConfig& config) override;
        void step(float delta_time, int collision_steps = 1, int integration_steps = 1) override;
        const char* backend_name() const override { return "Jolt"; }

        // --- rigid bodies ---
        RigidBodyHandle add_rigid_body(const RigidBodyDesc& desc) override;
        void remove_rigid_body(RigidBodyHandle handle) override;
        size_t get_body_count() const override;
        uint32_t get_active_body_count() const override;
        const RigidBodyStateSoA& get_body_states() const override { return body_state; }

        void apply_force(RigidBodyHandle handle, const bud::math::vec3& force) override;
        void apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) override;

        // --- articulations ---
        // Not implemented here on purpose: Jolt has no articulation solver, and the robot is being
        // moved to the MuJoCo backend through this same interface. Until that lands (the robot
        // layer still builds a Jolt ragdoll directly) these return "no articulation".
        ArticulationHandle create_articulation(const ArticulationDesc& desc) override;
        void remove_articulation(ArticulationHandle handle) override;
        bool get_articulation_state(ArticulationHandle handle, ArticulationStateSoA& out) const override;
        void set_articulation_target_angle(ArticulationHandle handle,
                                           const std::string& joint_name, float angle) override;
        void set_articulation_target_velocity(ArticulationHandle handle,
                                              const std::string& joint_name, float velocity) override;
        float get_articulation_joint_angle(ArticulationHandle handle,
                                           const std::string& joint_name) const override;
        float get_articulation_joint_stiffness(ArticulationHandle handle,
                                               const std::string& joint_name) const override;
        void set_articulation_link_transform(ArticulationHandle handle,
                                             const std::string& link_name,
                                             const bud::math::vec3& position,
                                             const bud::math::quaternion& rotation) override;
        void set_articulation_activated(ArticulationHandle handle, bool activated) override;

        // --- spatial queries ---
        std::optional<RaycastResult> raycast(const bud::math::vec3& from,
                                             const bud::math::vec3& to) const override;
        std::optional<ShapeCastResult> sphere_cast(const bud::math::vec3& from,
                                                   const bud::math::vec3& to,
                                                   float radius) const override;

        // --- contacts ---
        void set_contact_begin_callback(ContactCallback callback) override;
        void set_contact_persist_callback(ContactCallback callback) override;
        void set_contact_end_callback(ContactCallback callback) override;
        std::vector<ContactPoint> get_contacts() const override;

        // --- settings ---
        void set_gravity(const bud::math::vec3& gravity) override;
        bud::math::vec3 get_gravity() const override;

        // --- debug draw ---
        void collect_debug_lines(std::vector<DebugLine>& out_lines) const override;

        // ------------------------------------------------------------------
        // Jolt specific access (transitional)
        // ------------------------------------------------------------------
        // Still needed by the character controller and, until the robot moves to the MuJoCo
        // backend, by the robot loader. Keeping these explicit makes the remaining coupling
        // visible instead of hiding it behind the neutral interface.
        JPH::PhysicsSystem* get_jolt_system() const { return physics_system.get(); }
        JPH::TempAllocator* get_temp_allocator() const { return temp_allocator.get(); }

        void sync_to_jolt();
        void sync_from_jolt();

        size_t size() const { return body_count.load(std::memory_order_relaxed); }

        // Placeholder soft body storage (the cloth runs in our own GPU solver, not here).
        std::vector<bud::math::vec3> soft_body_positions;
        std::vector<float>           soft_body_masses;
        std::vector<uint8_t>         soft_body_flags;
        SoftBodyHandle create_soft_body(const SoftBodyDesc& desc);
        void           remove_soft_body(SoftBodyHandle handle);

        uint32_t get_total_body_count() const;

        // SoA accessors, kept inline (as before the move) so mirroring state does not pay a virtual
        // call per body. They write only the SoA; Jolt is updated at the next sync_to_jolt(), which
        // is exactly the ordering the old scene had.
        void set_body_position(RigidBodyHandle handle, const bud::math::vec3& pos) override {
            if (handle.is_valid() && handle.id < body_positions.size())
                body_positions[handle.id] = pos;
        }
        void set_body_rotation(RigidBodyHandle handle, const bud::math::quaternion& rot) override {
            if (handle.is_valid() && handle.id < body_rotations.size())
                body_rotations[handle.id] = rot;
        }
        void set_body_linear_velocity(RigidBodyHandle handle, const bud::math::vec3& vel) override {
            if (handle.is_valid() && handle.id < body_linear_velocities.size())
                body_linear_velocities[handle.id] = vel;
        }
        void set_body_angular_velocity(RigidBodyHandle handle, const bud::math::vec3& vel) override {
            if (handle.is_valid() && handle.id < body_angular_velocities.size())
                body_angular_velocities[handle.id] = vel;
        }

        void reset(size_t capacity) {
            body_state.resize(capacity);
            body_shapes.assign(capacity, nullptr);
            body_count.store(0);
            dropped_bodies.store(0);
        }

        std::mutex body_mutex;

        std::atomic<size_t> body_count{ 0 };
        std::atomic<size_t> dropped_bodies{ 0 };
        std::atomic<size_t> soft_body_count{ 0 };

    private:
        // Neutral SoA state plus name aliases, so the mirrored columns read the same way the old
        // scene code did. A GPU backend would keep the same arrays but never copy them to the CPU.
        RigidBodyStateSoA body_state;
        std::vector<bud::math::vec3>&       body_positions{ body_state.body_positions };
        std::vector<bud::math::quaternion>& body_rotations{ body_state.body_rotations };
        std::vector<bud::math::vec3>&       body_linear_velocities{ body_state.body_linear_velocities };
        std::vector<bud::math::vec3>&       body_angular_velocities{ body_state.body_angular_velocities };
        std::vector<bud::math::vec3>&       body_half_extents{ body_state.body_half_extents };
        std::vector<float>&                 body_masses{ body_state.body_masses };
        std::vector<float>&                 body_frictions{ body_state.body_frictions };
        std::vector<float>&                 body_restitutions{ body_state.body_restitutions };
        std::vector<uint8_t>&               body_flags{ body_state.body_flags };
        std::vector<void*>&                 body_user_data{ body_state.body_user_data };

        std::vector<JPH::Ref<JPH::Shape>> body_shapes;

        std::unique_ptr<JPH::PhysicsSystem> physics_system;

        // Listeners handed to Jolt as raw, non-owning pointers: this class owns and deletes them
        // (Jolt's PhysicsSystem destructor leaves them alone).
        JPH::ContactListener*        contact_listener = nullptr;
        JPH::BodyActivationListener* activation_listener = nullptr;

        std::unique_ptr<JPH::BroadPhaseLayerInterface>       broad_phase_layer_interface;
        std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter>  object_vs_broadphase_filter;
        std::unique_ptr<JPH::ObjectLayerPairFilter>          object_layer_pair_filter;

        std::unique_ptr<JPH::TempAllocator> temp_allocator;
        std::unique_ptr<JPH::JobSystem>     job_system;

        // Jolt body IDs, parallel to the SoA columns.
        std::vector<uint32_t> jolt_body_ids;

        ContactCallback contact_begin_cb;
        ContactCallback contact_persist_cb;
        ContactCallback contact_end_cb;

        bool initialized = false;
    };

} // namespace bud::physics
