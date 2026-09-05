#include "src/runtime/bud.character_controller.hpp"
#include "src/physics/bud.physics.scene.hpp"

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
        v.SetX(desired_velocity.x);
        v.SetZ(desired_velocity.z);
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
            bool ShouldCollide(JPH::BroadPhaseLayer) const override { return true; }
            bool ShouldCollide(JPH::ObjectLayer) const override { return true; }
            bool ShouldCollide(const JPH::BodyID&) const override { return true; }
            bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&) const override { return true; }
            bool ShouldCollide(const JPH::Shape*, const JPH::SubShapeID&, const JPH::Shape*, const JPH::SubShapeID&) const override { return true; }
        };
        AllFilter all_filter;

        character->ExtendedUpdate(dt, JPH::Vec3(gravity.x, gravity.y, gravity.z),
                                  update_settings, all_filter, all_filter,
                                  all_filter, all_filter, *physics_scene->get_temp_allocator());
    }

    void CharacterController::set_velocity(const bud::math::vec3& velocity) {
        desired_velocity = velocity;
    }

    bud::math::vec3 CharacterController::get_position() const {
        if (!character) return {};
        auto pos = character->GetPosition();
        return bud::math::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
    }

    bud::math::vec3 CharacterController::get_eye_position() const {
        if (!character) return {};
        auto pos = character->GetPosition();
        return bud::math::vec3(pos.GetX(), pos.GetY() + capsule_height * 0.5f + capsule_radius + 0.1f, pos.GetZ());
    }

    bool CharacterController::is_grounded() const {
        return character ? character->IsSupported() : false;
    }

    void CharacterController::teleport(const bud::math::vec3& pos) {
        if (character)
            character->SetPosition(JPH::RVec3(pos.x, pos.y, pos.z));
    }

    int CharacterController::unstick_from(const std::vector<bud::math::vec3>& body_positions,
                                          const std::vector<bud::math::vec3>& body_half_extents,
                                          float max_rise) {
        if (!character) return 0;

        // Only a capsule whose CENTRE is inside a static box is unrecoverable: Jolt's
        // penetration recovery copes with a head-high poke, but with the centre inside a
        // solid volume every direction is blocked and the character cannot move at all -
        // which looks exactly like "WASD stopped working".
        const size_t count = std::min(body_positions.size(), body_half_extents.size());
        auto engulfing = [&](const bud::math::vec3& center) {
            int hits = 0;
            for (size_t i = 0; i < count; ++i) {
                const auto d = body_positions[i] - center;
                const auto h = body_half_extents[i];
                if (std::fabs(d.x) <= h.x && std::fabs(d.y) <= h.y && std::fabs(d.z) <= h.z)
                    ++hits;
            }
            return hits;
        };

        const bud::math::vec3 start = get_position();
        const int stuck = engulfing(start);
        if (stuck == 0) return 0;

        constexpr float step = 0.1f;
        for (float rise = step; rise <= max_rise; rise += step) {
            const bud::math::vec3 lifted(start.x, start.y + rise, start.z);
            if (engulfing(lifted) == 0) {
                teleport(lifted);
                return stuck;
            }
        }
        return -stuck; // no free space found below max_rise
    }

} // namespace bud::scene