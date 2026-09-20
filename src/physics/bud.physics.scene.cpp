#include "src/physics/bud.physics.scene.hpp"

#include "src/core/bud.logger.hpp"
#include "src/physics/bud.physics.world.jolt.hpp"

namespace bud::physics {

    PhysicsScene::PhysicsScene() = default;

    PhysicsScene::~PhysicsScene() = default;

    void PhysicsScene::init(const PhysicsWorldConfig& config) {
        backend = config.backend;

        world = create_physics_world(backend);
        if (!world) {
            bud::eprint("[PhysicsScene] Failed to create a '{}' physics backend.",
                        backend == PhysicsBackend::Mujoco ? "MuJoCo" : "Jolt");
            return;
        }

        if (!world->init(config))
            bud::eprint("[PhysicsScene] Backend '{}' failed to initialise.", world->backend_name());

        body_count = 0;
    }

    void PhysicsScene::init(uint32_t max_bodies, uint32_t max_body_pairs,
                            uint32_t max_contact_constraints, PhysicsBackend in_backend) {
        PhysicsWorldConfig config;
        config.backend = in_backend;
        config.max_bodies = max_bodies;
        config.max_body_pairs = max_body_pairs;
        config.max_contact_constraints = max_contact_constraints;
        init(config);
    }

    void PhysicsScene::step(float delta_time, int collision_steps, int integration_steps) {
        if (!world)
            return;
        world->step(delta_time, collision_steps, integration_steps);
        body_count = world->get_body_count();
    }

    RigidBodyHandle PhysicsScene::add_rigid_body(const RigidBodyDesc& desc) {
        if (!world)
            return {};
        const RigidBodyHandle handle = world->add_rigid_body(desc);
        body_count = world->get_body_count();
        return handle;
    }

    void PhysicsScene::remove_rigid_body(RigidBodyHandle handle) {
        if (!world)
            return;
        world->remove_rigid_body(handle);
        body_count = world->get_body_count();
    }

    const RigidBodyStateSoA& PhysicsScene::get_body_states() const {
        static const RigidBodyStateSoA empty;
        return world ? world->get_body_states() : empty;
    }

    std::optional<RaycastResult> PhysicsScene::raycast(const bud::math::vec3& from,
                                                      const bud::math::vec3& to) const {
        if (!world)
            return std::nullopt;
        return world->raycast(from, to);
    }

    std::optional<ShapeCastResult> PhysicsScene::sphere_cast(const bud::math::vec3& from,
                                                             const bud::math::vec3& to,
                                                             float radius) const {
        if (!world)
            return std::nullopt;
        return world->sphere_cast(from, to, radius);
    }

    void PhysicsScene::set_contact_begin_callback(ContactCallback callback) {
        if (world)
            world->set_contact_begin_callback(std::move(callback));
    }

    void PhysicsScene::set_contact_persist_callback(ContactCallback callback) {
        if (world)
            world->set_contact_persist_callback(std::move(callback));
    }

    void PhysicsScene::set_contact_end_callback(ContactCallback callback) {
        if (world)
            world->set_contact_end_callback(std::move(callback));
    }

    void PhysicsScene::set_gravity(const bud::math::vec3& gravity) {
        if (world)
            world->set_gravity(gravity);
    }

    bud::math::vec3 PhysicsScene::get_gravity() const {
        return world ? world->get_gravity() : bud::math::vec3(0.0f);
    }

    uint32_t PhysicsScene::get_active_body_count() const {
        return world ? world->get_active_body_count() : 0u;
    }

    uint32_t PhysicsScene::get_total_body_count() const {
        return world ? static_cast<uint32_t>(world->get_body_count()) : 0u;
    }

    void PhysicsScene::sync_to_jolt() {
        // Writes already land in the backend's SoA; the Jolt backend mirrors the SoA into Jolt at
        // the start of step(). Kept so existing call sites (one) stay unchanged.
    }

    JPH::PhysicsSystem* PhysicsScene::get_jolt_system() const {
        const auto* jolt = dynamic_cast<const JoltPhysicsWorld*>(world.get());
        if (!jolt) {
            if (world)
                bud::eprint("[PhysicsScene] get_jolt_system() called on the '{}' backend.",
                            world->backend_name());
            return nullptr;
        }
        return jolt->get_jolt_system();
    }

    JPH::TempAllocator* PhysicsScene::get_temp_allocator() const {
        const auto* jolt = dynamic_cast<const JoltPhysicsWorld*>(world.get());
        if (!jolt)
            return nullptr;
        return jolt->get_temp_allocator();
    }

} // namespace bud::physics
