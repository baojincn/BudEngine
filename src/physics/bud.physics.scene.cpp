#include "src/physics/bud.physics.scene.hpp"
#include "src/core/bud.logger.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>

namespace {

    constexpr uint32_t LAYER_STATIC  = 0;
    constexpr uint32_t LAYER_DYNAMIC = 1;
    constexpr uint32_t NUM_OBJECT_LAYERS = 2;

    constexpr uint32_t BROAD_LAYER_STATIC = 0;
    constexpr uint32_t BROAD_LAYER_DYNAMIC = 1;
    constexpr uint32_t NUM_BROAD_LAYERS = 2;

    class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
    public:
        BPLayerInterface() {
            m_map[LAYER_STATIC]  = BROAD_LAYER_STATIC;
            m_map[LAYER_DYNAMIC] = BROAD_LAYER_DYNAMIC;
        }
        uint32_t GetNumBroadPhaseLayers() const override { return NUM_BROAD_LAYERS; }
        JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
            JPH_ASSERT(layer < NUM_OBJECT_LAYERS);
            return JPH::BroadPhaseLayer(m_map[layer]);
        }
    private:
        uint8_t m_map[NUM_OBJECT_LAYERS];
    };

    class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override { return true; }
    };

    class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override { return true; }
    };

    class ContactListenerImpl final : public JPH::ContactListener {
    public:
        using Callback = bud::physics::PhysicsScene::ContactCallback;

        Callback on_begin;
        Callback on_persist;
        Callback on_end;

        JPH::ValidateResult OnContactValidate(const JPH::Body&, const JPH::Body&,
                                              JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        void OnContactAdded(const JPH::Body& body_a, const JPH::Body& body_b,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
            if (on_begin) {
                bud::math::vec3 normal(manifold.mWorldSpaceNormal.GetX(),
                                       manifold.mWorldSpaceNormal.GetY(),
                                       manifold.mWorldSpaceNormal.GetZ());
                for (size_t i = 0; i < manifold.mRelativeContactPointsOn1.size(); ++i) {
                    auto pt = manifold.GetWorldSpaceContactPointOn1(i);
                    on_begin(reinterpret_cast<void*>(body_a.GetUserData()),
                             reinterpret_cast<void*>(body_b.GetUserData()),
                             bud::math::vec3(pt.GetX(), pt.GetY(), pt.GetZ()),
                             normal, manifold.mPenetrationDepth);
                }
            }
        }

        void OnContactPersisted(const JPH::Body& body_a, const JPH::Body& body_b,
                                const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
            if (on_persist) {
                bud::math::vec3 normal(manifold.mWorldSpaceNormal.GetX(),
                                       manifold.mWorldSpaceNormal.GetY(),
                                       manifold.mWorldSpaceNormal.GetZ());
                for (size_t i = 0; i < manifold.mRelativeContactPointsOn1.size(); ++i) {
                    auto pt = manifold.GetWorldSpaceContactPointOn1(i);
                    on_persist(reinterpret_cast<void*>(body_a.GetUserData()),
                               reinterpret_cast<void*>(body_b.GetUserData()),
                               bud::math::vec3(pt.GetX(), pt.GetY(), pt.GetZ()),
                               normal, manifold.mPenetrationDepth);
                }
            }
        }

        void OnContactRemoved(const JPH::SubShapeIDPair&) override {
            if (on_end) {
                on_end(nullptr, nullptr, bud::math::vec3(0.0f), bud::math::vec3(0.0f), 0.0f);
            }
        }
    };

    class ActivationListenerImpl final : public JPH::BodyActivationListener {
    public:
        void OnBodyActivated(const JPH::BodyID&, uint64_t) override {}
        void OnBodyDeactivated(const JPH::BodyID&, uint64_t) override {}
    };

    JPH::RVec3 to_jolt(const bud::math::vec3& v) {
        return JPH::RVec3(v.x, v.y, v.z);
    }

    JPH::Quat to_jolt(const bud::math::quaternion& q) {
        return JPH::Quat(q.x, q.y, q.z, q.w);
    }

    bud::math::vec3 from_jolt(JPH::RVec3Arg v) {
        return bud::math::vec3(v.GetX(), v.GetY(), v.GetZ());
    }

    bud::math::quaternion from_jolt(JPH::QuatArg q) {
        return bud::math::quaternion(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }

    JPH::EMotionType to_jolt_motion(uint8_t flags) {
        if (flags & bud::physics::PhysicsScene::BODY_FLAG_KINEMATIC)
            return JPH::EMotionType::Kinematic;
        if (flags & bud::physics::PhysicsScene::BODY_FLAG_STATIC)
            return JPH::EMotionType::Static;
        return JPH::EMotionType::Dynamic;
    }

    JPH::ObjectLayer to_jolt_layer(uint8_t flags) {
        return (flags & bud::physics::PhysicsScene::BODY_FLAG_STATIC) ? LAYER_STATIC : LAYER_DYNAMIC;
    }

    JPH::Shape* create_jolt_shape(const bud::physics::ShapeDesc& desc) {
        switch (desc.type) {
            case bud::physics::ShapeType::Sphere:
                return new JPH::SphereShape(desc.radius);
            case bud::physics::ShapeType::Box:
                return new JPH::BoxShape(JPH::Vec3(desc.half_extent.x, desc.half_extent.y, desc.half_extent.z));
            case bud::physics::ShapeType::Capsule:
                return new JPH::CapsuleShape(desc.capsule_half_height, desc.capsule_radius);
            case bud::physics::ShapeType::ConvexHull: {
                JPH::Array<JPH::Vec3> pts;
                pts.reserve(desc.vertices.size());
                for (const auto& v : desc.vertices)
                    pts.push_back(JPH::Vec3(v.x, v.y, v.z));
                JPH::ConvexHullShapeSettings settings(pts, JPH::cDefaultConvexRadius);
                JPH::Shape::ShapeResult result;
                auto* shape = new JPH::ConvexHullShape(settings, result);
                if (result.HasError())
                    bud::eprint("[PhysicsScene] ConvexHullShape creation error: {}", result.GetError());
                return shape;
            }
            case bud::physics::ShapeType::Mesh: {
                if (!desc.indices.empty()) {
                    JPH::VertexList verts;
                    verts.reserve(desc.vertices.size());
                    for (const auto& v : desc.vertices)
                        verts.push_back(JPH::Float3(v.x, v.y, v.z));
                    JPH::IndexedTriangleList tris;
                    tris.reserve(desc.indices.size() / 3);
                    for (size_t i = 0; i + 2 < desc.indices.size(); i += 3)
                        tris.push_back({desc.indices[i], desc.indices[i + 1], desc.indices[i + 2], 0});
                    JPH::MeshShapeSettings settings(verts, tris);
                    JPH::Shape::ShapeResult result;
                    auto* shape = new JPH::MeshShape(settings, result);
                    if (result.HasError())
                        bud::eprint("[PhysicsScene] MeshShape creation error: {}", result.GetError());
                    return shape;
                }
                JPH::TriangleList tris;
                tris.reserve(desc.vertices.size() / 3);
                for (size_t i = 0; i + 2 < desc.vertices.size(); i += 3)
                    tris.push_back({JPH::Float3(desc.vertices[i].x, desc.vertices[i].y, desc.vertices[i].z),
                                    JPH::Float3(desc.vertices[i + 1].x, desc.vertices[i + 1].y, desc.vertices[i + 1].z),
                                    JPH::Float3(desc.vertices[i + 2].x, desc.vertices[i + 2].y, desc.vertices[i + 2].z),
                                    0});
                JPH::MeshShapeSettings settings(tris);
                JPH::Shape::ShapeResult result;
                auto* shape = new JPH::MeshShape(settings, result);
                if (result.HasError())
                    bud::eprint("[PhysicsScene] MeshShape creation error: {}", result.GetError());
                return shape;
            }
            default:
                return nullptr;
        }
    }
}

namespace bud::physics {

    PhysicsScene::PhysicsScene() = default;

    PhysicsScene::~PhysicsScene() {
        if (!initialized) return;

        if (physics_system) {
            auto& bi = physics_system->GetBodyInterface();
            const size_t n = body_count.load(std::memory_order_relaxed);
            const size_t limit = std::min(n, jolt_body_ids.size());
            for (size_t i = 0; i < limit; ++i) {
                JPH::BodyID id(jolt_body_ids[i]);
                if (id.IsInvalid()) continue;
                bi.RemoveBody(id);
                bi.DestroyBody(id);
            }
        }

        for (auto* shape : jolt_shapes) {
            if (shape) shape->Release();
        }
        jolt_shapes.clear();
        jolt_body_ids.clear();

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    void PhysicsScene::init(uint32_t max_bodies, uint32_t max_body_pairs,
                            uint32_t max_contact_constraints) {
        if (initialized) return;

        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

        job_system = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            std::max(1u, std::thread::hardware_concurrency() - 1u));

        broad_phase_layer_interface = std::make_unique<BPLayerInterface>();
        object_vs_broadphase_filter = std::make_unique<ObjectVsBroadPhaseLayerFilter>();
        object_layer_pair_filter = std::make_unique<ObjectLayerPairFilter>();

        physics_system = std::make_unique<JPH::PhysicsSystem>();
        physics_system->Init(max_bodies, 0, max_body_pairs, max_contact_constraints,
                               *broad_phase_layer_interface,
                               *object_vs_broadphase_filter,
                               *object_layer_pair_filter);

        physics_system->SetContactListener(new ContactListenerImpl());
        physics_system->SetBodyActivationListener(new ActivationListenerImpl());
        physics_system->SetGravity(JPH::Vec3(0.0f, -9.80665f, 0.0f));

        initialized = true;
        bud::print("[PhysicsScene] Initialized (max_bodies={}, max_pairs={}, max_contacts={})",
                   max_bodies, max_body_pairs, max_contact_constraints);
    }

    void PhysicsScene::sync_to_jolt() {
        if (!initialized || !physics_system) return;

        auto& bi = physics_system->GetBodyInterface();
        const size_t n = body_count.load(std::memory_order_relaxed);

        jolt_body_ids.resize(n, JPH::BodyID::cInvalidBodyID);
        jolt_shapes.resize(n, nullptr);

        for (size_t i = 0; i < n; ++i) {
            JPH::BodyID id(jolt_body_ids[i]);
            if (id.IsInvalid()) {
                auto* shape = new JPH::BoxShape(JPH::Vec3(0.5f, 0.5f, 0.5f));
                auto motion = to_jolt_motion(body_flags[i]);
                JPH::BodyCreationSettings bcs(shape, to_jolt(body_positions[i]),
                                              to_jolt(body_rotations[i]),
                                              motion, to_jolt_layer(body_flags[i]));
                bcs.mFriction = 0.5f;
                bcs.mRestitution = 0.3f;
                bcs.mAllowSleeping = (body_flags[i] & BODY_FLAG_ALLOW_SLEEP) != 0;
                bcs.mIsSensor = (body_flags[i] & BODY_FLAG_SENSOR) != 0;
                if (body_flags[i] & BODY_FLAG_CCD)
                    bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;

                if (motion == JPH::EMotionType::Static) {
                    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateMassAndInertia;
                } else {
                    bcs.mMassPropertiesOverride.mMass = body_masses[i];
                    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                }

                auto* body = bi.CreateBody(bcs);
                if (body) {
                    body->SetUserData(reinterpret_cast<uint64_t>(body_user_data[i]));
                    bi.AddBody(body->GetID(), JPH::EActivation::Activate);
                    jolt_body_ids[i] = body->GetID().GetIndexAndSequenceNumber();
                    jolt_shapes[i] = shape;
                } else {
                    shape->Release();
                }
            } else {
                JPH::BodyLockWrite lock(physics_system->GetBodyLockInterface(), id);
                if (lock.Succeeded()) {
                    auto& body = lock.GetBody();
                    bi.SetPositionAndRotation(id, to_jolt(body_positions[i]),
                                              to_jolt(body_rotations[i]),
                                              JPH::EActivation::DontActivate);
                    bi.SetLinearVelocity(id, JPH::Vec3(body_linear_velocities[i].x,
                                                        body_linear_velocities[i].y,
                                                        body_linear_velocities[i].z));
                    bi.SetAngularVelocity(id, JPH::Vec3(body_angular_velocities[i].x,
                                                         body_angular_velocities[i].y,
                                                         body_angular_velocities[i].z));
                }
            }
        }
    }

    void PhysicsScene::sync_from_jolt() {
        if (!initialized || !physics_system) return;

        const size_t n = body_count.load(std::memory_order_relaxed);
        const size_t limit = std::min(n, jolt_body_ids.size());
        for (size_t i = 0; i < limit; ++i) {
            JPH::BodyID id(jolt_body_ids[i]);
            if (id.IsInvalid()) continue;

            JPH::BodyLockRead lock(physics_system->GetBodyLockInterface(), id);
            if (lock.Succeeded()) {
                const auto& body = lock.GetBody();
                body_positions[i] = from_jolt(body.GetPosition());
                body_rotations[i] = from_jolt(body.GetRotation());
                auto lv = body.GetLinearVelocity();
                body_linear_velocities[i] = bud::math::vec3(lv.GetX(), lv.GetY(), lv.GetZ());
                auto av = body.GetAngularVelocity();
                body_angular_velocities[i] = bud::math::vec3(av.GetX(), av.GetY(), av.GetZ());
            }
        }
    }

    void PhysicsScene::step(float delta_time, int collision_steps, int integration_steps) {
        if (!initialized || !physics_system) return;

        sync_to_jolt();

        auto err = physics_system->Update(delta_time, collision_steps,
                                             temp_allocator.get(), job_system.get());
        if (err != JPH::EPhysicsUpdateError::None)
            bud::eprint("[PhysicsScene] Step returned error: {}", static_cast<int>(err));

        sync_from_jolt();
    }

    void PhysicsScene::remove_rigid_body(RigidBodyHandle handle) {
        if (!handle.is_valid() || handle.id >= jolt_body_ids.size()) return;

        JPH::BodyID id(jolt_body_ids[handle.id]);
        if (!id.IsInvalid()) {
            auto& bi = physics_system->GetBodyInterface();
            bi.RemoveBody(id);
            bi.DestroyBody(id);
        }

        if (jolt_shapes[handle.id]) {
            jolt_shapes[handle.id]->Release();
            jolt_shapes[handle.id] = nullptr;
        }
        jolt_body_ids[handle.id] = JPH::BodyID::cInvalidBodyID;
    }

    void PhysicsScene::apply_force(RigidBodyHandle handle, const bud::math::vec3& force) {
        if (!handle.is_valid() || handle.id >= jolt_body_ids.size()) return;
        JPH::BodyID id(jolt_body_ids[handle.id]);
        if (id.IsInvalid()) return;
        physics_system->GetBodyInterface().AddForce(id, JPH::Vec3(force.x, force.y, force.z));
    }

    void PhysicsScene::apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) {
        if (!handle.is_valid() || handle.id >= jolt_body_ids.size()) return;
        JPH::BodyID id(jolt_body_ids[handle.id]);
        if (id.IsInvalid()) return;
        physics_system->GetBodyInterface().AddImpulse(id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }

    SoftBodyHandle PhysicsScene::create_soft_body(const SoftBodyDesc& desc) {
        size_t idx = soft_body_count.fetch_add(1, std::memory_order_relaxed);
        if (idx >= soft_body_positions.size()) {
            soft_body_positions.push_back(desc.vertices.empty() ? bud::math::vec3(0.0f) : desc.vertices[0]);
            soft_body_masses.push_back(desc.mass);
            soft_body_flags.push_back(desc.is_skinned ? 1u : 0u);
        } else {
            soft_body_positions[idx] = desc.vertices.empty() ? bud::math::vec3(0.0f) : desc.vertices[0];
            soft_body_masses[idx] = desc.mass;
            soft_body_flags[idx] = desc.is_skinned ? 1u : 0u;
        }
        bud::print("[PhysicsScene] Soft body created (id={}) — placeholder.", idx);
        return {static_cast<uint32_t>(idx)};
    }

    void PhysicsScene::remove_soft_body(SoftBodyHandle) {}

    std::optional<RaycastResult> PhysicsScene::raycast(const bud::math::vec3& from,
                                                        const bud::math::vec3& to) const {
        if (!initialized || !physics_system) return std::nullopt;

        JPH::RRayCast ray(to_jolt(from), JPH::Vec3(to.x - from.x, to.y - from.y, to.z - from.z));
        JPH::RayCastResult hit;
        if (physics_system->GetNarrowPhaseQuery().CastRay(ray, hit)) {
            const JPH::BodyLockRead lock(physics_system->GetBodyLockInterface(), hit.mBodyID);
            if (lock.Succeeded()) {
                const auto& body = lock.GetBody();
                return RaycastResult{
                    true,
                    hit.mFraction,
                    from_jolt(ray.GetPointOnRay(hit.mFraction)),
                    bud::math::vec3(hit.mFraction > 0.0f ? 0.0f : 1.0f),
                    reinterpret_cast<void*>(body.GetUserData())
                };
            }
        }
        return RaycastResult{false, 1.0f, to, {}, nullptr};
    }

    std::optional<ShapeCastResult> PhysicsScene::sphere_cast(const bud::math::vec3& from,
                                                              const bud::math::vec3& to,
                                                              float radius) const {
        if (!initialized || !physics_system) return std::nullopt;

        JPH::SphereShape sphere(radius);
        JPH::RShapeCast shape_cast(
            &sphere, JPH::Vec3::sReplicate(1.0f),
            JPH::RMat44::sTranslation(to_jolt(from)),
            JPH::Vec3(to.x - from.x, to.y - from.y, to.z - from.z));

        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        JPH::ShapeCastSettings settings;
        physics_system->GetNarrowPhaseQuery().CastShape(
            shape_cast, settings, JPH::RVec3::sZero(), collector);

        if (collector.HadHit()) {
            const auto& hit = collector.mHit;
            return ShapeCastResult{
                true,
                hit.mFraction,
                from_jolt(shape_cast.GetPointOnRay(hit.mFraction)),
                from_jolt(hit.mPenetrationAxis.Normalized()),
                nullptr
            };
        }
        return ShapeCastResult{false, 1.0f, to, {}, nullptr};
    }

    void PhysicsScene::set_contact_begin_callback(ContactCallback cb) {
        contact_begin_cb = std::move(cb);
        if (initialized) {
            auto* cl = static_cast<ContactListenerImpl*>(physics_system->GetContactListener());
            if (cl) cl->on_begin = contact_begin_cb;
        }
    }

    void PhysicsScene::set_contact_persist_callback(ContactCallback cb) {
        contact_persist_cb = std::move(cb);
        if (initialized) {
            auto* cl = static_cast<ContactListenerImpl*>(physics_system->GetContactListener());
            if (cl) cl->on_persist = contact_persist_cb;
        }
    }

    void PhysicsScene::set_contact_end_callback(ContactCallback cb) {
        contact_end_cb = std::move(cb);
        if (initialized) {
            auto* cl = static_cast<ContactListenerImpl*>(physics_system->GetContactListener());
            if (cl) cl->on_end = contact_end_cb;
        }
    }

    void PhysicsScene::set_gravity(const bud::math::vec3& gravity) {
        if (physics_system)
            physics_system->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
    }

    bud::math::vec3 PhysicsScene::get_gravity() const {
        if (physics_system) {
            auto g = physics_system->GetGravity();
            return bud::math::vec3(g.GetX(), g.GetY(), g.GetZ());
        }
        return bud::math::vec3(0.0f, -9.80665f, 0.0f);
    }

    uint32_t PhysicsScene::get_active_body_count() const {
        return physics_system ? physics_system->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
    }

    uint32_t PhysicsScene::get_total_body_count() const {
        return physics_system ? physics_system->GetNumBodies() : 0;
    }

} // namespace bud::physics