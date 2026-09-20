#include "src/runtime/bud.character_controller.hpp"
#include "src/physics/bud.physics.scene.hpp"
#include "src/core/bud.logger.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Body/BodyFilter.h>

#include <algorithm>
#include <cmath>

namespace bud::scene {

    CharacterController::CharacterController() = default;

    CharacterController::~CharacterController() {
        delete character;
    }

    void CharacterController::init(bud::physics::PhysicsScene* physics, const bud::math::vec3& start_pos,
                                   float radius, float height) {
        physics_scene = physics;
        capsule_radius = radius;
        capsule_height = height;
        fallback_position = start_pos;

        // The player character is Jolt's CharacterVirtual. A non-Jolt backend (the MuJoCo robot
        // world) runs no player character: robot simulation is the subject there. Say so once instead
        // of letting get_jolt_system() report an error on a backend that never had a character.
        if (physics && physics->get_backend() != bud::physics::PhysicsBackend::Jolt) {
            bud::print("[CharacterController] player character disabled: backend is {}",
                       physics->get_backend() == bud::physics::PhysicsBackend::Mujoco ? "MuJoCo" : "non-Jolt");
            return;
        }

        auto* system = physics ? physics->get_jolt_system() : nullptr;
        if (!system) return;

        JPH::Ref<JPH::CapsuleShape> shape = new JPH::CapsuleShape(height * 0.5f, radius);
        JPH::CharacterVirtualSettings settings;
        settings.mShape = shape;
        settings.mUp = JPH::Vec3(0.0f, 1.0f, 0.0f);
        settings.mMaxSlopeAngle = JPH::DegreesToRadians(45.0f);
        settings.mMass = 70.0f;
        settings.mMaxStrength = 100.0f;
        settings.mEnhancedInternalEdgeRemoval = true;

        JPH::RVec3 pos(start_pos.x, start_pos.y, start_pos.z);
        character = new JPH::CharacterVirtual(&settings, pos, JPH::Quat::sIdentity(), system);
    }

    void CharacterController::update(float dt) {
        if (!character || !physics_scene) return;

        JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;
        update_settings.mStickToFloorStepDown = JPH::Vec3(0.0f, -0.5f, 0.0f);
        update_settings.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.4f, 0.0f);

        // Velocity contract of JPH::CharacterVirtual (CharacterVirtual.h):
        // "note it's your own responsibility to apply gravity to the character velocity".
        // The gravity passed to ExtendedUpdate() only presses the character onto the body
        // it stands on; it is never integrated into mLinearVelocity. So the plain
        // "SetLinearVelocity(desired)" pattern makes the character hover forever - it only
        // ever descends by the stick-to-floor probe distance. The game authors the
        // horizontal component; Y keeps the character's own accumulated velocity plus
        // this step's gravity.
        JPH::Vec3 v = character->GetLinearVelocity();

        // Realistic ground friction and inertia simulation
        constexpr float kGroundFriction = 14.0f; // Responsive deceleration on floor
        constexpr float kAirControl = 3.0f;

        if (character->IsSupported()) {
            float factor = std::clamp(kGroundFriction * dt, 0.0f, 1.0f);
            v.SetX(std::lerp(v.GetX(), desired_velocity.x, factor));
            v.SetZ(std::lerp(v.GetZ(), desired_velocity.z, factor));
        } else {
            float factor = std::clamp(kAirControl * dt, 0.0f, 1.0f);
            v.SetX(std::lerp(v.GetX(), desired_velocity.x, factor));
            v.SetZ(std::lerp(v.GetZ(), desired_velocity.z, factor));
        }

        v.SetY(v.GetY() + gravity.y * dt);

        constexpr float kMaxFallSpeed = 60.0f;
        if (v.GetY() < -kMaxFallSpeed)
            v.SetY(-kMaxFallSpeed);
        // Resting on the floor: drop accumulated downward speed so gravity does not fight
        // the stick-to-floor / walk-stairs probes on steps and slopes.
        if (character->IsSupported() && v.GetY() < 0.0f)
            v.SetY(0.0f);

        character->SetLinearVelocity(v);

        class AllFilter : public JPH::BroadPhaseLayerFilter, public JPH::ObjectLayerFilter, public JPH::BodyFilter, public JPH::ShapeFilter {
        public:
            const std::vector<uint32_t>* ignored = nullptr;
            bool ShouldCollide(JPH::BroadPhaseLayer) const override { return true; }
            bool ShouldCollide(JPH::ObjectLayer) const override { return true; }
            bool ShouldCollide(const JPH::BodyID& id) const override {
                if (ignored && !ignored->empty()) {
                    uint32_t raw_id = id.GetIndexAndSequenceNumber();
                    for (uint32_t ign : *ignored) {
                        if (ign == raw_id)
                            return false;
                    }
                }
                return true;
            }
            bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&) const override { return true; }
            bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&, const JPH::Shape*, const JPH::SubShapeID&) const override { return true; }
        };
        AllFilter all_filter;
        all_filter.ignored = &ignored_bodies;

        character->ExtendedUpdate(dt, JPH::Vec3(gravity.x, gravity.y, gravity.z),
                                  update_settings, all_filter, all_filter,
                                  all_filter, all_filter, *physics_scene->get_temp_allocator());

        // Wedge detector (log-only): if movement input is held but the capsule cannot
        // move at all, it is embedded in some static proxy volume. Automatic lifting
        // is unsafe next to AABB proxies - a tall phantom box would catapult the
        // character onto an invisible slab above - so this only reports.
        const JPH::RVec3 now_pos = character->GetPosition();
        const float now_x = static_cast<float>(now_pos.GetX());
        const float now_z = static_cast<float>(now_pos.GetZ());
        const bool wants_move = std::fabs(desired_velocity.x) > 0.05f || std::fabs(desired_velocity.z) > 0.05f;
        const float moved_xz = std::fabs(now_x - last_pos.x) + std::fabs(now_z - last_pos.z);
        if (wants_move && moved_xz < 0.002f)
            ++stuck_frames;
        else
            stuck_frames = 0;
        if (stuck_frames == 20)
            bud::print("[Character] movement input held but the capsule cannot move - possible wedge");
        last_pos = bud::math::vec3(now_x, static_cast<float>(now_pos.GetY()), now_z);
    }

    void CharacterController::set_velocity(const bud::math::vec3& velocity) {
        desired_velocity = velocity;
        fallback_velocity = velocity;
    }

    bud::math::vec3 CharacterController::get_position() const {
        if (!character)
            return fallback_position;
        auto pos = character->GetPosition();
        return bud::math::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
    }

    bud::math::vec3 CharacterController::get_linear_velocity() const {
        if (!character)
            return fallback_velocity;
        auto vel = character->GetLinearVelocity();
        return bud::math::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
    }

    bud::math::vec3 CharacterController::get_eye_position() const {
        if (!character)
            return fallback_position + bud::math::vec3(0.0f, 1.5f, 0.0f);
        auto pos = character->GetPosition();
        // UE5 ACharacter::BaseEyeHeight = 150cm above the character's feet.
        // Feet = shape center - (half cylinder height + radius).
        const float feet_y = static_cast<float>(pos.GetY()) - (capsule_height * 0.5f + capsule_radius);
        return bud::math::vec3(static_cast<float>(pos.GetX()), feet_y + 1.5f, static_cast<float>(pos.GetZ()));
    }

    bool CharacterController::is_grounded() const {
        return character ? character->IsSupported() : false;
    }

    void CharacterController::teleport(const bud::math::vec3& pos) {
        fallback_position = pos;
        if (character)
            character->SetPosition(JPH::RVec3(pos.x, pos.y, pos.z));
    }

    bool CharacterController::snap_to_ground() {
        if (!character || !physics_scene)
            return false;

        const float r = capsule_radius;
        const float feet_offset = capsule_height * 0.5f + r;
        const bud::math::vec3 pos = get_position();

        // Raycast downwards from slightly above the capsule to find the real floor surface
        bud::math::vec3 ray_start(pos.x, pos.y + 0.5f, pos.z);
        bud::math::vec3 ray_end(pos.x, pos.y - 15.0f, pos.z);

        auto hit = physics_scene->raycast(ray_start, ray_end);
        if (hit.has_value() && hit->hit) {
            float ground_y = hit->hit_point.y;
            constexpr float k_ground_margin = 0.002f;
            teleport(bud::math::vec3(pos.x, ground_y + k_ground_margin + feet_offset, pos.z));
            return true;
        }

        return false;
    }

} // namespace bud::scene