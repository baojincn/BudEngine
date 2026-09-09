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
    }

    bud::math::vec3 CharacterController::get_position() const {
        if (!character)
            return {};
        auto pos = character->GetPosition();
        return bud::math::vec3(pos.GetX(), pos.GetY(), pos.GetZ());
    }

    bud::math::vec3 CharacterController::get_linear_velocity() const {
        if (!character)
            return bud::math::vec3(0.0f);
        auto vel = character->GetLinearVelocity();
        return bud::math::vec3(vel.GetX(), vel.GetY(), vel.GetZ());
    }

    bud::math::vec3 CharacterController::get_eye_position() const {
        if (!character) return {};
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
        if (character)
            character->SetPosition(JPH::RVec3(pos.x, pos.y, pos.z));
    }

    void CharacterController::snap_to_ground(const std::vector<bud::math::vec3>& body_positions,
                                             const std::vector<bud::math::vec3>& body_half_extents) {
        if (!character) return;
        const size_t count = std::min(body_positions.size(), body_half_extents.size());
        if (count == 0) return;

        const float r = capsule_radius;
        const float feet_offset = capsule_height * 0.5f + r;
        const bud::math::vec3 start = get_position();
        const float head_y = start.y + feet_offset; // capsule top at the spawn point

        // ALL support surfaces at the ORIGINAL spawn XZ below the head, highest first.
        std::vector<float> tops;
        for (size_t i = 0; i < count; ++i) {
            const auto& p = body_positions[i];
            const auto& h = body_half_extents[i];
            if (std::fabs(start.x - p.x) > h.x || std::fabs(start.z - p.z) > h.z)
                continue;
            const float top = p.y + h.y;
            if (top <= head_y)
                tops.push_back(top);
        }
        std::sort(tops.begin(), tops.end(), std::greater<float>());

        auto overlap_count = [&](float feet_y) {
            const float cy = feet_y + feet_offset;
            int hits = 0;
            for (size_t i = 0; i < count; ++i) {
                const auto& p = body_positions[i];
                const auto& h = body_half_extents[i];
                if (std::fabs(start.x - p.x) > h.x + r) continue;
                if (std::fabs(start.z - p.z) > h.z + r) continue;
                if (std::fabs(cy - p.y) > h.y + feet_offset) continue;
                ++hits;
            }
            return hits;
        };

        // Keep the original XZ; adjust ONLY the height. Try the support surfaces from
        // the highest down: the first one with a fully clear capsule wins (a ledge
        // just below head height fails its own headroom check, the real floor below
        // passes). The character is never moved sideways.
        for (float top : tops) {
            const float feet_y = top + 0.05f;
            if (overlap_count(feet_y) == 0) {
                teleport(bud::math::vec3(start.x, feet_y + feet_offset, start.z));
                return;
            }
        }

        // Tight shaft: keep the original XZ and take the LEAST-overlapping height so
        // Jolt's own penetration recovery has the best chance.
        int best_hits = static_cast<int>(count) + 1;
        float best_feet = start.y - feet_offset;
        for (float top : tops) {
            const float feet_y = top + 0.05f;
            const int hits = overlap_count(feet_y);
            if (hits < best_hits) {
                best_hits = hits;
                best_feet = feet_y;
            }
        }
        teleport(bud::math::vec3(start.x, best_feet + feet_offset, start.z));
    }

    int CharacterController::unstick_from(const std::vector<bud::math::vec3>& body_positions,
                                          const std::vector<bud::math::vec3>& body_half_extents,
                                          float max_rise) {
        if (!character) return 0;

        // Jolt's CharacterVirtual blocks ALL movement casts when the shape starts
        // overlapping a static body ("WASD stopped working"), and its penetration
        // recovery only copes with shallow pokes. Lift until the WHOLE capsule
        // bounding box (radius x total height) is clear of every static AABB - a
        // center-only check misses the common case where the feet are embedded in
        // a thin floor slab / stair step while the centre is still free.
        const size_t count = std::min(body_positions.size(), body_half_extents.size());
        const float r = capsule_radius;
        const float half_h = capsule_height * 0.5f + r; // total capsule half height
        auto overlapping = [&](const bud::math::vec3& center) {
            int hits = 0;
            for (size_t i = 0; i < count; ++i) {
                const auto d = body_positions[i] - center;
                const auto h = body_half_extents[i];
                if (std::fabs(d.x) <= h.x + r &&
                    std::fabs(d.y) <= h.y + half_h &&
                    std::fabs(d.z) <= h.z + r)
                    ++hits;
            }
            return hits;
        };

        const bud::math::vec3 start = get_position();
        const int stuck = overlapping(start);
        if (stuck == 0) return 0;

        constexpr float step = 0.1f;
        for (float rise = step; rise <= max_rise; rise += step) {
            const bud::math::vec3 lifted(start.x, start.y + rise, start.z);
            if (overlapping(lifted) == 0) {
                teleport(lifted);
                return stuck;
            }
        }
        return -stuck; // no free space found below max_rise
    }

} // namespace bud::scene