#include "src/physics/bud.physics.world.jolt.hpp"
#include "src/core/bud.logger.hpp"

#include <atomic>
#include <mutex>
#include <vector>

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
#include <Jolt/Core/IssueReporting.h>

#include <cstdarg>
#include <cmath>

namespace {

    // ------------------------------------------------------------------
    // Jolt diagnostics -> engine logger.
    //
    // Jolt reports asserts/trace output through the JPH::Trace / JPH::AssertFailed
    // function pointers, whose default implementation uses printf + OutputDebugString.
    // A GUI process without a console shows nothing, so a failed assert looks like a
    // silent exit with status 0x80000003. Routing them into bud::print makes the real
    // expression, file and line visible in the log.
    // ------------------------------------------------------------------
    void jolt_trace(const char* in_format, ...) {
        char buf[1024];
        va_list args;
        va_start(args, in_format);
        vsnprintf(buf, sizeof(buf), in_format, args);
        va_end(args);
        bud::print("[Jolt] {}", buf);
    }

    bool jolt_assert_failed(const char* in_expression, const char* in_message,
                            const char* in_file, JPH::uint in_line) {
        bud::print("[Jolt] ASSERT FAILED: {} ({}:{})",
                   in_expression ? in_expression : "?",
                   in_file ? in_file : "?", in_line);
        if (in_message && in_message[0] != '\0')
            bud::print("[Jolt]   message: {}", in_message);
        if (auto* g = bud::get_global_logger()) g->flush();
        // Return false = "do not break". Returning true makes Jolt execute a breakpoint,
        // which is an unhandled exception (hard crash) when no debugger is attached - the
        // log line above is what we actually want, and execution should continue so the
        // behaviour matches the release build.
        return false;
    }

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
        // Static objects never collide with anything in the static broad-phase
        // layer: they are immovable and their proxies intentionally overlap
        // (hollow-shell slabs, adjacent wall AABBs). Without this, static-static
        // pairs flood the narrow phase with deep convex-vs-mesh penetrations (EPA)
        // and overflow the body-pair buffer (assert in debug Jolt, heap corruption
        // in release Jolt - the release-only crash).
        bool ShouldCollide(JPH::ObjectLayer inObjectLayer, JPH::BroadPhaseLayer inBroadPhaseLayer) const override {
            if (inObjectLayer == LAYER_STATIC && inBroadPhaseLayer == JPH::BroadPhaseLayer(BROAD_LAYER_STATIC))
                return false;
            return true;
        }
    };

    class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
    public:
        bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::ObjectLayer inLayer2) const override {
            // Jolt's intended semantics: static bodies never collide with each other.
            if (inLayer1 == LAYER_STATIC && inLayer2 == LAYER_STATIC)
                return false;

            // TODO(jolt-epa): the G1 ragdoll is the only dynamic body here and its
            // convex/box contacts with the baked CollisionLOD triangle meshes make Jolt's
            // EPA run away (debug: JPH_ASSERT(inTolerance >= FLT_EPSILON) at
            // EPAPenetrationDepth.h:154; release: out-of-bounds write at a fixed 0xFxxx
            // offset, i.e. EPA's internal pool index running away). Clamping the
            // penetration tolerance did not stop it, so the corruption is upstream of
            // that assert. Until the real culprit is found, dynamic bodies do not pair
            // with static ones. CharacterVirtual is unaffected: it performs its own shape
            // casts and never consults this pair filter, so the player still walks on the
            // world. Remove this block once the Jolt issue is fixed.
            constexpr bool kDisableDynamicVsStatic = true;
            if (kDisableDynamicVsStatic && inLayer1 != inLayer2)
                return false;

            return true;
        }
    };

    class ContactListenerImpl final : public JPH::ContactListener {
    public:
        using Callback = bud::physics::ContactCallback;

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
        if (flags & bud::physics::BODY_FLAG_KINEMATIC)
            return JPH::EMotionType::Kinematic;
        if (flags & bud::physics::BODY_FLAG_STATIC)
            return JPH::EMotionType::Static;
        return JPH::EMotionType::Dynamic;
    }

    JPH::ObjectLayer to_jolt_layer(uint8_t flags) {
        return (flags & bud::physics::BODY_FLAG_STATIC) ? LAYER_STATIC : LAYER_DYNAMIC;
    }

    JPH::Ref<JPH::Shape> create_jolt_shape(const bud::physics::ShapeDesc& desc) {
        switch (desc.type) {
            case bud::physics::ShapeType::Sphere:
                return new JPH::SphereShape(desc.radius);
            case bud::physics::ShapeType::Box: {
                constexpr float cMinHalfExtent = 0.005f;
                auto clamp_extent = [](float v) {
                    return (v > cMinHalfExtent && v == v) ? v : cMinHalfExtent;
                };
                return new JPH::BoxShape(JPH::Vec3(clamp_extent(desc.half_extent.x),
                                                   clamp_extent(desc.half_extent.y),
                                                   clamp_extent(desc.half_extent.z)));
            }
            case bud::physics::ShapeType::Capsule:
                return new JPH::CapsuleShape(desc.capsule_half_height, desc.capsule_radius);
            case bud::physics::ShapeType::ConvexHull: {
                if (desc.vertices.empty())
                    return nullptr;
                JPH::Array<JPH::Vec3> pts;
                pts.reserve(desc.vertices.size());
                for (const auto& v : desc.vertices)
                    pts.push_back(JPH::Vec3(v.x, v.y, v.z));
                JPH::ConvexHullShapeSettings settings(pts, JPH::cDefaultConvexRadius);
                auto result = settings.Create();
                if (result.HasError()) {
                    bud::eprint("[PhysicsScene] ConvexHullShape creation error: {}", result.GetError());
                    return nullptr;
                }
                return result.Get();
            }
            case bud::physics::ShapeType::Mesh: {
                if (desc.vertices.empty())
                    return nullptr;
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
                    auto result = settings.Create();
                    if (result.HasError()) {
                        bud::eprint("[PhysicsScene] MeshShape creation error: {}", result.GetError());
                        return nullptr;
                    }
                    return result.Get();
                }
                JPH::TriangleList tris;
                tris.reserve(desc.vertices.size() / 3);
                for (size_t i = 0; i + 2 < desc.vertices.size(); i += 3)
                    tris.push_back({JPH::Float3(desc.vertices[i].x, desc.vertices[i].y, desc.vertices[i].z),
                                    JPH::Float3(desc.vertices[i + 1].x, desc.vertices[i + 1].y, desc.vertices[i + 1].z),
                                    JPH::Float3(desc.vertices[i + 2].x, desc.vertices[i + 2].y, desc.vertices[i + 2].z),
                                    0});
                JPH::MeshShapeSettings settings(tris);
                auto result = settings.Create();
                if (result.HasError()) {
                    bud::eprint("[PhysicsScene] MeshShape creation error: {}", result.GetError());
                    return nullptr;
                }
                return result.Get();
            }
            default:
                return nullptr;
        }
    }
}

namespace bud::physics {

    RigidBodyHandle JoltPhysicsWorld::add_rigid_body(const RigidBodyDesc& desc) {
        std::scoped_lock lock(body_mutex);
        const size_t idx = body_count.load(std::memory_order_relaxed);
        if (idx >= body_positions.size()) [[unlikely]] {
            dropped_bodies.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        body_positions[idx] = desc.position;
        body_rotations[idx] = desc.rotation;
        body_linear_velocities[idx] = bud::math::vec3(0.0f);
        if (desc.shape.type == ShapeType::Box)
            body_half_extents[idx] = desc.shape.half_extent;
        else
            body_half_extents[idx] = bud::math::vec3(0.0f);
        body_frictions[idx] = desc.material.friction;
        body_restitutions[idx] = desc.material.restitution;
        body_user_data[idx] = nullptr;

        body_flags[idx] = static_cast<uint8_t>(
            (desc.motion_type == MotionType::Static    ? BODY_FLAG_STATIC    : 0) |
            (desc.motion_type == MotionType::Kinematic ? BODY_FLAG_KINEMATIC : 0) |
            (desc.is_sensor    ? BODY_FLAG_SENSOR      : 0) |
            (desc.is_ccd       ? BODY_FLAG_CCD         : 0) |
            (desc.allow_sleep  ? BODY_FLAG_ALLOW_SLEEP : 0));

        if (idx < body_shapes.size())
            body_shapes[idx] = create_jolt_shape(desc.shape);

        body_count.store(idx + 1, std::memory_order_release);
        return {static_cast<uint32_t>(idx)};
    }

    JoltPhysicsWorld::JoltPhysicsWorld() = default;

    JoltPhysicsWorld::~JoltPhysicsWorld() {
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

        jolt_body_ids.clear();

        // Tear the system down before releasing the listeners it points at, then drop
        // the process-wide Jolt registration.
        physics_system.reset();
        delete contact_listener;
        contact_listener = nullptr;
        delete activation_listener;
        activation_listener = nullptr;

        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    bool JoltPhysicsWorld::init(const bud::physics::PhysicsWorldConfig& config) {
        if (initialized) return true;

        const uint32_t max_bodies = config.max_bodies;
        const uint32_t max_body_pairs = config.max_body_pairs;
        const uint32_t max_contact_constraints = config.max_contact_constraints;

        // Allocate the SoA columns BEFORE anything can call add_rigid_body().
        // Without this every body is silently dropped: add_rigid_body() rejects
        // indices >= body_positions.size(), and an un-reset scene has size 0.
        reset(max_bodies);

        // Install Jolt's diagnostics BEFORE any Jolt code runs. RegisterTypes() itself
        // emits Trace() calls; with the default handler still in place those hit Jolt's
        // DummyTrace, whose JPH_ASSERT(false) aborts the process (previously invisible
        // because our AssertFailed handler was never registered without
        // JPH_ENABLE_ASSERTS, so the message went to OutputDebugString instead).
        JPH::Trace = &jolt_trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &jolt_assert_failed;)

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

        // Jolt's PhysicsSystem does NOT own (nor delete) these listeners, so the
        // PhysicsScene keeps the pointers and releases them in its destructor.
        contact_listener = new ContactListenerImpl();
        physics_system->SetContactListener(contact_listener);
        activation_listener = new ActivationListenerImpl();
        physics_system->SetBodyActivationListener(activation_listener);
        physics_system->SetGravity(JPH::Vec3(config.gravity.x, config.gravity.y, config.gravity.z));

        initialized = true;
        bud::print("[PhysicsScene] Initialized (max_bodies={}, max_pairs={}, max_contacts={})",
                   max_bodies, max_body_pairs, max_contact_constraints);
        return true;
    }

    void JoltPhysicsWorld::sync_to_jolt() {
        if (!initialized || !physics_system) return;

        auto& bi = physics_system->GetBodyInterface();
        const size_t n = body_count.load(std::memory_order_relaxed);

        jolt_body_ids.resize(n, JPH::BodyID::cInvalidBodyID);

        bool new_bodies = false;
        const size_t count = std::min({n, body_positions.size(), body_rotations.size(), body_flags.size(), body_half_extents.size()});
        for (size_t i = 0; i < count; ++i) {
            const auto& position = body_positions[i];
            const auto& rotation = body_rotations[i];
            const auto& linear_velocity = body_linear_velocities[i];
            const auto& angular_velocity = body_angular_velocities[i];
            const auto& half_extent = body_half_extents[i];
            const bool finite_state =
                std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
                std::isfinite(rotation.w) && std::isfinite(rotation.x) &&
                std::isfinite(rotation.y) && std::isfinite(rotation.z) &&
                std::isfinite(linear_velocity.x) && std::isfinite(linear_velocity.y) &&
                std::isfinite(linear_velocity.z) && std::isfinite(angular_velocity.x) &&
                std::isfinite(angular_velocity.y) && std::isfinite(angular_velocity.z) &&
                std::isfinite(half_extent.x) && std::isfinite(half_extent.y) &&
                std::isfinite(half_extent.z);
            if (!finite_state) {
                bud::eprint("[Physics] Non-finite SoA state before Jolt body {}: "
                            "position=({}, {}, {}), rotation=({}, {}, {}, {}), "
                            "linear_velocity=({}, {}, {}), angular_velocity=({}, {}, {}), "
                            "half_extent=({}, {}, {})",
                            i, position.x, position.y, position.z,
                            rotation.w, rotation.x, rotation.y, rotation.z,
                            linear_velocity.x, linear_velocity.y, linear_velocity.z,
                            angular_velocity.x, angular_velocity.y, angular_velocity.z,
                            half_extent.x, half_extent.y, half_extent.z);
                continue;
            }

            JPH::BodyID id(jolt_body_ids[i]);
            if (id.IsInvalid()) {
                JPH::Ref<JPH::Shape> shape = (i < body_shapes.size()) ? body_shapes[i] : nullptr;
                if (!shape) {
                    auto he = body_half_extents[i];
                    // Meshes that are planar (floors, walls) produce a world AABB with a
                    // zero-width axis. Jolt can reject such a box and, worse, penetration
                    // recovery cannot resolve a zero-thickness surface, so clamp to a thin slab.
                    constexpr float cMinHalfExtent = 0.005f;
                    auto clamp_extent = [](float v) {
                        return (v > cMinHalfExtent && v == v) ? v : cMinHalfExtent;
                    };
                    shape = new JPH::BoxShape(JPH::Vec3(clamp_extent(he.x),
                                                        clamp_extent(he.y),
                                                        clamp_extent(he.z)));
                }
                auto motion = to_jolt_motion(body_flags[i]);
                JPH::BodyCreationSettings bcs(shape, to_jolt(body_positions[i]),
                                              to_jolt(body_rotations[i]),
                                              motion, to_jolt_layer(body_flags[i]));
                bcs.mFriction = (i < body_frictions.size()) ? body_frictions[i] : 0.5f;
                bcs.mRestitution = (i < body_restitutions.size()) ? body_restitutions[i] : 0.1f;
                bcs.mAllowSleeping = (body_flags[i] & BODY_FLAG_ALLOW_SLEEP) != 0;
                bcs.mIsSensor = (body_flags[i] & BODY_FLAG_SENSOR) != 0;
                if (body_flags[i] & BODY_FLAG_CCD)
                    bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;

                if (motion == JPH::EMotionType::Static) {
                    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
                    bcs.mMassPropertiesOverride.mMass = 0.0f;
                    bcs.mMassPropertiesOverride.mInertia = JPH::Mat44::sZero();
                } else {
                    bcs.mMassPropertiesOverride.mMass = body_masses[i];
                    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                }

                auto* body = bi.CreateBody(bcs);
                if (body) {
                    body->SetUserData(reinterpret_cast<uint64_t>(body_user_data[i]));
                    bi.AddBody(body->GetID(), JPH::EActivation::Activate);
                    jolt_body_ids[i] = body->GetID().GetIndexAndSequenceNumber();
                    new_bodies = true;
                }
            } else {
                // BodyInterface locks the body internally. Never wrap these calls in a
                // BodyLockWrite of our own: Jolt's per-body mutexes are not recursive, so
                // locking the same body twice from the same thread trips its deadlock
                // check (and would deadlock outright in a build without asserts).
                bi.SetPositionAndRotation(id, to_jolt(body_positions[i]),
                                          to_jolt(body_rotations[i]),
                                          JPH::EActivation::DontActivate);

                // Static bodies own no motion properties - setting their velocity is an
                // assert inside Jolt. Only dynamic/kinematic bodies carry velocity.
                if (!(body_flags[i] & BODY_FLAG_STATIC)) {
                    bi.SetLinearVelocity(id, JPH::Vec3(body_linear_velocities[i].x,
                                                        body_linear_velocities[i].y,
                                                        body_linear_velocities[i].z));
                    bi.SetAngularVelocity(id, JPH::Vec3(body_angular_velocities[i].x,
                                                         body_angular_velocities[i].y,
                                                         body_angular_velocities[i].z));
                }
            }
        }

        size_t invalid_ids = 0;
        for (size_t i = 0; i < jolt_body_ids.size(); ++i) {
            if (JPH::BodyID(jolt_body_ids[i]).IsInvalid())
                ++invalid_ids;
        }
        if (invalid_ids != 0)
            bud::eprint("[Physics] {} invalid Jolt body IDs before Update (body_count={}, id_count={})",
                        invalid_ids, n, jolt_body_ids.size());

        if (new_bodies)
            physics_system->OptimizeBroadPhase();
    }

    void JoltPhysicsWorld::sync_from_jolt() {
        if (!initialized || !physics_system) return;

        const size_t n = body_count.load(std::memory_order_relaxed);
        const size_t limit = std::min({n, jolt_body_ids.size(), body_positions.size()});
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

    void JoltPhysicsWorld::step(float delta_time, int collision_steps, int integration_steps) {
        if (!initialized || !physics_system) return;

        std::scoped_lock lock(body_mutex);
        sync_to_jolt();

        auto err = physics_system->Update(delta_time, collision_steps,
                                             temp_allocator.get(), job_system.get());
        if (err != JPH::EPhysicsUpdateError::None)
            bud::eprint("[PhysicsScene] Step returned error: {}", static_cast<int>(err));

        sync_from_jolt();
    }

    void JoltPhysicsWorld::remove_rigid_body(RigidBodyHandle handle) {
        std::scoped_lock lock(body_mutex);
        if (!handle.is_valid() || handle.id >= jolt_body_ids.size())
            return;

        if (handle.id < body_shapes.size())
            body_shapes[handle.id] = nullptr;

        JPH::BodyID id(jolt_body_ids[handle.id]);
        if (!id.IsInvalid()) {
            auto& bi = physics_system->GetBodyInterface();
            bi.RemoveBody(id);
            bi.DestroyBody(id);
        }

        jolt_body_ids[handle.id] = JPH::BodyID::cInvalidBodyID;
    }

    void JoltPhysicsWorld::apply_force(RigidBodyHandle handle, const bud::math::vec3& force) {
        if (!handle.is_valid() || handle.id >= jolt_body_ids.size()) return;
        JPH::BodyID id(jolt_body_ids[handle.id]);
        if (id.IsInvalid()) return;
        physics_system->GetBodyInterface().AddForce(id, JPH::Vec3(force.x, force.y, force.z));
    }

    void JoltPhysicsWorld::apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) {
        if (!handle.is_valid() || handle.id >= jolt_body_ids.size()) return;
        JPH::BodyID id(jolt_body_ids[handle.id]);
        if (id.IsInvalid()) return;
        physics_system->GetBodyInterface().AddImpulse(id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
    }

    SoftBodyHandle JoltPhysicsWorld::create_soft_body(const SoftBodyDesc& desc) {
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

    void JoltPhysicsWorld::remove_soft_body(SoftBodyHandle) {}

    std::optional<RaycastResult> JoltPhysicsWorld::raycast(const bud::math::vec3& from,
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

    std::optional<ShapeCastResult> JoltPhysicsWorld::sphere_cast(const bud::math::vec3& from,
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

    void JoltPhysicsWorld::set_contact_begin_callback(ContactCallback cb) {
        contact_begin_cb = std::move(cb);
        if (initialized) {
            auto* cl = static_cast<ContactListenerImpl*>(physics_system->GetContactListener());
            if (cl) cl->on_begin = contact_begin_cb;
        }
    }

    void JoltPhysicsWorld::set_contact_persist_callback(ContactCallback cb) {
        contact_persist_cb = std::move(cb);
        if (initialized) {
            auto* cl = static_cast<ContactListenerImpl*>(physics_system->GetContactListener());
            if (cl) cl->on_persist = contact_persist_cb;
        }
    }

    void JoltPhysicsWorld::set_contact_end_callback(ContactCallback cb) {
        contact_end_cb = std::move(cb);
        if (initialized) {
            auto* cl = static_cast<ContactListenerImpl*>(physics_system->GetContactListener());
            if (cl) cl->on_end = contact_end_cb;
        }
    }

    void JoltPhysicsWorld::set_gravity(const bud::math::vec3& gravity) {
        if (physics_system)
            physics_system->SetGravity(JPH::Vec3(gravity.x, gravity.y, gravity.z));
    }

    bud::math::vec3 JoltPhysicsWorld::get_gravity() const {
        if (physics_system) {
            auto g = physics_system->GetGravity();
            return bud::math::vec3(g.GetX(), g.GetY(), g.GetZ());
        }
        return bud::math::vec3(0.0f, -9.80665f, 0.0f);
    }

    uint32_t JoltPhysicsWorld::get_active_body_count() const {
        return physics_system ? physics_system->GetNumActiveBodies(JPH::EBodyType::RigidBody) : 0;
    }

    uint32_t JoltPhysicsWorld::get_total_body_count() const {
        return physics_system ? physics_system->GetNumBodies() : 0;
    }

    // get_jolt_system() / get_temp_allocator() are defined inline in the header.

    // ------------------------------------------------------------------
    // Articulations: deliberately unimplemented in the Jolt backend.
    // ------------------------------------------------------------------
    // Jolt has no articulation (generalised coordinate) solver and its joint motors are soft
    // springs, so the robot is being moved onto the MuJoCo backend through this same interface.
    // Until the robot layer stops building a Jolt ragdoll directly, these exist only to satisfy the
    // interface and report "no articulation" instead of pretending to work.

    ArticulationHandle JoltPhysicsWorld::create_articulation(const ArticulationDesc&) {
        bud::eprint("[JoltPhysicsWorld] create_articulation: Jolt has no articulation solver; "
                    "use the MuJoCo backend for robots.");
        return ArticulationHandle{};
    }

    void JoltPhysicsWorld::remove_articulation(ArticulationHandle) {}

    bool JoltPhysicsWorld::get_articulation_state(ArticulationHandle, ArticulationStateSoA&) const {
        return false;
    }

    void JoltPhysicsWorld::set_articulation_target_angle(ArticulationHandle, const std::string&, float) {}

    void JoltPhysicsWorld::set_articulation_target_velocity(ArticulationHandle, const std::string&, float) {}

    float JoltPhysicsWorld::get_articulation_joint_angle(ArticulationHandle, const std::string&) const {
        return 0.0f;
    }

    float JoltPhysicsWorld::get_articulation_joint_stiffness(ArticulationHandle, const std::string&) const {
        return 0.0f;
    }

    void JoltPhysicsWorld::set_articulation_link_transform(ArticulationHandle, const std::string&,
                                                          const bud::math::vec3&, const bud::math::quaternion&) {}

    void JoltPhysicsWorld::set_articulation_activated(ArticulationHandle, bool) {}

    size_t JoltPhysicsWorld::get_body_count() const {
        return size();
    }

    // ------------------------------------------------------------------
    // Contacts / debug
    // ------------------------------------------------------------------
    std::vector<ContactPoint> JoltPhysicsWorld::get_contacts() const {
        // TODO(physics abstraction): record the listener's contacts into a reusable buffer so RL
        // observation and debugging can read them. The callbacks above already deliver them.
        return {};
    }

    void JoltPhysicsWorld::collect_debug_lines(std::vector<DebugLine>&) const {
        // TODO(physics abstraction): emit body boxes / contact points here instead of having the
        // engine read the SoA columns directly.
    }

} // namespace bud::physics
