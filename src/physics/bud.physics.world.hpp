#pragma once

// Backend neutral physics interface.
//
// Why this exists: Jolt and MuJoCo are good at different things and the engine wants both.
//   - Jolt  : game worlds. Triangle mesh levels, tens of thousands of rigid bodies, already
//             coupled to our cloth and rendering paths.
//   - MuJoCo: robots. Generalised coordinate joints, a real actuator model (kp/kv, gear,
//             force range, armature, Coulomb friction) and a parameterised contact model -
//             which is what "accurate motor simulation" needs, and what Jolt structurally
//             cannot provide (it has no articulation solver and its joints are soft springs).
// A future GPU XPBD world (rigid + soft in one compute shader solver) becomes a third backend
// implementing this same interface.
//
// Layout rules that keep that door open:
//   - Body and articulation state is Struct-of-Arrays indexed by a dense index, never an object
//     graph: the same buffers can be uploaded to the GPU and read by a compute solver without
//     reshaping, and the renderer can consume them directly.
//   - Nothing in this header exposes a backend type (no JPH::, no mjModel). Backend specific
//     detail stays inside the implementations.
//   - Callers talk in physical units (metres, kilograms, radians, N*m/rad), because that is what
//     transfer to hardware requires.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>

#include "src/core/bud.math.hpp"
#include "src/physics/bud.physics.types.hpp"

namespace bud::physics {

    enum class PhysicsBackend : uint8_t {
        Jolt,      // game world
        Mujoco,    // robot world
        GpuXpbd,   // future: unified rigid + soft XPBD world on the GPU
    };

    struct PhysicsWorldConfig {
        PhysicsBackend backend = PhysicsBackend::Jolt;
        uint32_t max_bodies = 65536;
        uint32_t max_body_pairs = 65536;
        uint32_t max_contact_constraints = 10240;
        bud::math::vec3 gravity{ 0.0f, -9.80665f, 0.0f };
        // Root used to resolve content-relative asset paths referenced by cooked models (e.g. the
        // meshes a cooked MuJoCo model asks for). Backends that need no assets ignore it.
        std::string asset_root;
    };

    // ------------------------------------------------------------------
    // Rigid body state, SoA. GPU friendly by construction.
    // ------------------------------------------------------------------
    enum BodyFlags : uint8_t {
        BODY_FLAG_NONE        = 0,
        BODY_FLAG_STATIC      = 1 << 0,
        BODY_FLAG_KINEMATIC   = 1 << 1,
        BODY_FLAG_SENSOR      = 1 << 2,
        BODY_FLAG_CCD         = 1 << 3,
        BODY_FLAG_ALLOW_SLEEP = 1 << 4,
    };

    struct RigidBodyStateSoA {
        std::vector<bud::math::vec3>       body_positions;
        std::vector<bud::math::quaternion> body_rotations;
        std::vector<bud::math::vec3>       body_linear_velocities;
        std::vector<bud::math::vec3>       body_angular_velocities;
        std::vector<bud::math::vec3>       body_half_extents;
        std::vector<float>                 body_masses;
        std::vector<float>                 body_frictions;
        std::vector<float>                 body_restitutions;
        std::vector<uint8_t>               body_flags;
        std::vector<void*>                 body_user_data;

        // Number of populated entries. Backends that manage their own counter (Jolt keeps an atomic
        // one) simply leave this alone.
        size_t body_count = 0;

        void resize(size_t capacity) {
            body_positions.assign(capacity, bud::math::vec3(0.0f));
            body_rotations.assign(capacity, bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f));
            body_linear_velocities.assign(capacity, bud::math::vec3(0.0f));
            body_angular_velocities.assign(capacity, bud::math::vec3(0.0f));
            body_half_extents.assign(capacity, bud::math::vec3(0.5f));
            body_masses.assign(capacity, 0.0f);
            body_frictions.assign(capacity, 0.5f);
            body_restitutions.assign(capacity, 0.1f);
            body_flags.assign(capacity, 0);
            body_user_data.assign(capacity, nullptr);
        }
    };

    // ------------------------------------------------------------------
    // Articulations: a tree of linked bodies with joints. This is the robot.
    // ------------------------------------------------------------------
    struct ArticulationHandle {
        uint32_t id = ~0u;
        bool is_valid() const { return id != ~0u; }
    };

    enum class JointKind : uint8_t {
        Revolute,   // 1 DOF hinge with limits
        Continuous, // 1 DOF hinge without limits
        Prismatic,  // 1 DOF slider
        Fixed,      // welded
    };

    // ------------------------------------------------------------------
    // Cooked backend model (produced offline, shipped inside .budasset)
    // ------------------------------------------------------------------
    // Format tag is backend neutral on purpose: a future GPU XPBD world would add its own case
    // rather than adopting MuJoCo's.
    enum class CookedModelFormat : uint32_t {
        MjcfText = 0,  // MuJoCo MJCF XML
        MjbBinary = 1, // MuJoCo compiled binary model
    };

    // One mesh the cooked model names, and the asset that provides its bytes. Meshes stay logical
    // references so a mesh used for rendering and collision exists once in the package.
    struct CookedModelMeshRef {
        std::string model_name; // name the model uses, e.g. "meshes/pelvis.STL"
        std::string asset_path; // content-relative asset providing it
    };

    struct CookedPhysicsModel {
        CookedModelFormat format = CookedModelFormat::MjcfText;
        std::vector<uint8_t> payload;
        std::vector<CookedModelMeshRef> meshes;
        // Read-only metadata baked by the cook: render/animation data the model format drops (e.g.
        // URDF joint velocity limits) and physics parameters it cannot express (actuator gains,
        // armature, solver/contact settings).
        std::string render_metadata_json;
        std::string physics_metadata_json;

        bool is_valid() const { return !payload.empty(); }
    };

    struct ArticulationLinkDesc {
        std::string name;
        // Local pose relative to the parent link (its own inertial frame). The backend composes
        // them with the root pose, so no world transform has to be precomputed here.
        bud::math::vec3       local_position{ 0.0f };
        bud::math::quaternion local_rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        float                 mass = 0.0f;
        bud::math::vec3       center_of_mass{ 0.0f };
        ShapeDesc             shape; // Collision shape (baked convex hull or primitive)
        bool                  has_collision = true;
    };

    struct ArticulationJointDesc {
        std::string name;
        std::string parent_link;
        std::string child_link;
        JointKind   kind = JointKind::Revolute;
        bud::math::vec3 origin{ 0.0f };              // joint frame in the parent link
        bud::math::vec3 axis{ 0.0f, 0.0f, 1.0f };    // rotation/translation axis in the child link
        float lower_limit = 0.0f;
        float upper_limit = 0.0f;

        // Actuator model, in physical units. Real motor data and sim-to-real work are expressed
        // this way (a position servo is kp * angle_error + kv * rate_error, torque limited).
        float stiffness = 0.0f;   // N*m/rad (or N/m for a prismatic joint)
        float damping = 0.0f;     // N*m/(rad/s)
        float max_torque = 0.0f;  // N*m, the hardware limit
    };

    struct ArticulationDesc {
        std::string name;
        std::string root_link;
        bud::math::vec3       root_position{ 0.0f };
        bud::math::quaternion root_rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        std::vector<ArticulationLinkDesc>  links;
        std::vector<ArticulationJointDesc> joints;
        // Spawn pose: joint angles the articulation is *built* in, so nothing has to move on the
        // first frames (a robot created in its bind pose and then commanded into a stance gets
        // shoved off its feet by its own motors).
        std::unordered_map<std::string, float> initial_joint_angles;
        // Cooked backend-specific model, straight out of the robot's .budasset (see
        // CookedPhysicsModel). A backend that compiles its own model format (MuJoCo) loads this
        // instead of rebuilding anything from the neutral description above, which keeps the asset
        // the single source of truth for robot physics data.
        CookedPhysicsModel cooked_model;
    };

    struct ArticulationStateSoA {
        std::vector<bud::math::vec3>       link_positions;
        std::vector<bud::math::quaternion> link_rotations;
        std::vector<bud::math::vec3>       link_linear_velocities;
        std::vector<bud::math::vec3>       link_angular_velocities;
        std::vector<float>                 joint_positions;
        std::vector<float>                 joint_velocities;
        std::vector<float>                 joint_torques;

        void resize(size_t link_capacity, size_t joint_capacity) {
            link_positions.assign(link_capacity, bud::math::vec3(0.0f));
            link_rotations.assign(link_capacity, bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f));
            link_linear_velocities.assign(link_capacity, bud::math::vec3(0.0f));
            link_angular_velocities.assign(link_capacity, bud::math::vec3(0.0f));
            joint_positions.assign(joint_capacity, 0.0f);
            joint_velocities.assign(joint_capacity, 0.0f);
            joint_torques.assign(joint_capacity, 0.0f);
        }
    };

    // ------------------------------------------------------------------
    // Contacts
    // ------------------------------------------------------------------
    struct ContactPoint {
        void*           body_a_user_data = nullptr;
        void*           body_b_user_data = nullptr;
        bud::math::vec3 point{ 0.0f };
        bud::math::vec3 normal{ 0.0f };
        float           penetration_depth = 0.0f;
    };

    using ContactCallback = std::function<void(void* body_a_user_data,
                                                void* body_b_user_data,
                                                const bud::math::vec3& contact_point,
                                                const bud::math::vec3& contact_normal,
                                                float penetration_depth)>;

    struct DebugLine {
        bud::math::vec3 a{ 0.0f };
        bud::math::vec3 b{ 0.0f };
        bud::math::vec3 color{ 1.0f, 1.0f, 1.0f };
    };

    // ------------------------------------------------------------------
    // The world
    // ------------------------------------------------------------------
    class PhysicsWorldBase {
    public:
        virtual ~PhysicsWorldBase() = default;

        virtual bool init(const PhysicsWorldConfig& config) = 0;
        virtual void step(float delta_time, int collision_steps = 1, int integration_steps = 1) = 0;
        virtual const char* backend_name() const = 0;

        // --- rigid bodies ---
        virtual RigidBodyHandle add_rigid_body(const RigidBodyDesc& desc) = 0;
        virtual void remove_rigid_body(RigidBodyHandle handle) = 0;
        virtual size_t get_body_count() const = 0;
        virtual uint32_t get_active_body_count() const = 0;
        virtual const RigidBodyStateSoA& get_body_states() const = 0;

        virtual void set_body_position(RigidBodyHandle handle, const bud::math::vec3& position) = 0;
        virtual void set_body_rotation(RigidBodyHandle handle, const bud::math::quaternion& rotation) = 0;
        virtual void set_body_linear_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) = 0;
        virtual void set_body_angular_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) = 0;
        virtual void apply_force(RigidBodyHandle handle, const bud::math::vec3& force) = 0;
        virtual void apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) = 0;

        // --- articulations (robots) ---
        virtual ArticulationHandle create_articulation(const ArticulationDesc& desc) = 0;
        virtual void remove_articulation(ArticulationHandle handle) = 0;
        virtual bool get_articulation_state(ArticulationHandle handle, ArticulationStateSoA& out) const = 0;
        virtual void set_articulation_target_angle(ArticulationHandle handle,
                                                   const std::string& joint_name, float angle) = 0;
        virtual void set_articulation_target_velocity(ArticulationHandle handle,
                                                      const std::string& joint_name, float velocity) = 0;
        virtual float get_articulation_joint_angle(ArticulationHandle handle,
                                                   const std::string& joint_name) const = 0;
        virtual float get_articulation_joint_stiffness(ArticulationHandle handle,
                                                       const std::string& joint_name) const = 0;
        // Teleport a single link (spawn placement correction, RL resets).
        virtual void set_articulation_link_transform(ArticulationHandle handle,
                                                     const std::string& link_name,
                                                     const bud::math::vec3& position,
                                                     const bud::math::quaternion& rotation) = 0;
        virtual void set_articulation_activated(ArticulationHandle handle, bool activated) = 0;

        // --- spatial queries ---
        virtual std::optional<RaycastResult> raycast(const bud::math::vec3& from,
                                                     const bud::math::vec3& to) const = 0;
        virtual std::optional<ShapeCastResult> sphere_cast(const bud::math::vec3& from,
                                                           const bud::math::vec3& to,
                                                           float radius) const = 0;

        // --- contacts ---
        virtual void set_contact_begin_callback(ContactCallback callback) = 0;
        virtual void set_contact_persist_callback(ContactCallback callback) = 0;
        virtual void set_contact_end_callback(ContactCallback callback) = 0;
        // Contact snapshot for logging, RL observation and debugging.
        virtual std::vector<ContactPoint> get_contacts() const = 0;

        // --- settings ---
        virtual void set_gravity(const bud::math::vec3& gravity) = 0;
        virtual bud::math::vec3 get_gravity() const = 0;

        // --- debug draw ---
        virtual void collect_debug_lines(std::vector<DebugLine>& out_lines) const = 0;
    };

    // Backend factory. The implementation files register themselves here, so the engine can pick a
    // backend per project/scene (or per future GPU world) without knowing which ones exist.
    std::unique_ptr<PhysicsWorldBase> create_physics_world(PhysicsBackend backend);

} // namespace bud::physics
