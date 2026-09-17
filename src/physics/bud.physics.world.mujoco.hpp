#pragma once

// MuJoCo implementation of PhysicsWorldBase.
//
// MuJoCo is the robot backend: generalised coordinate joints, an explicit actuator model (kp/kv,
// gear, force range, armature, Coulomb friction) and a parameterised contact model. That is what
// accurate motor simulation needs and what Jolt structurally cannot provide, so robots run here
// while Jolt keeps the game world.
//
// The world is a single mjModel: scene bodies added through add_rigid_body and robots loaded from
// their cooked .budasset payload are combined into one spec and compiled once (lazily, on first
// step/query). The cooked payload is what the offline pipeline produced - the backend never parses
// a URDF, and the mesh bytes MuJoCo asks for are served straight out of our own mesh assets.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/physics/bud.physics.world.hpp"

struct mjModel_;
struct mjData_;
struct mjSpec_;

namespace bud::physics {

    class MujocoPhysicsWorld final : public PhysicsWorldBase {
    public:
        MujocoPhysicsWorld();
        ~MujocoPhysicsWorld() override;

        MujocoPhysicsWorld(const MujocoPhysicsWorld&) = delete;
        MujocoPhysicsWorld& operator=(const MujocoPhysicsWorld&) = delete;

        bool init(const PhysicsWorldConfig& config) override;
        void step(float delta_time, int collision_steps = 1, int integration_steps = 1) override;
        const char* backend_name() const override { return "MuJoCo"; }

        // --- rigid bodies (scene geometry: floor, props) ---
        RigidBodyHandle add_rigid_body(const RigidBodyDesc& desc) override;
        void remove_rigid_body(RigidBodyHandle handle) override;
        size_t get_body_count() const override;
        uint32_t get_active_body_count() const override;
        const RigidBodyStateSoA& get_body_states() const override;

        void set_body_position(RigidBodyHandle handle, const bud::math::vec3& position) override;
        void set_body_rotation(RigidBodyHandle handle, const bud::math::quaternion& rotation) override;
        void set_body_linear_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) override;
        void set_body_angular_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) override;
        void apply_force(RigidBodyHandle handle, const bud::math::vec3& force) override;
        void apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) override;

        // --- articulations (robots) ---
        ArticulationHandle create_articulation(const ArticulationDesc& desc) override;
        void remove_articulation(ArticulationHandle handle) override;
        bool get_articulation_state(ArticulationHandle handle, ArticulationStateSoA& out) const override;
        void set_articulation_joint_commands(ArticulationHandle handle,
                                             std::span<const JointCommand> commands) override;
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
        void set_articulation_activated(ArticulationHandle, bool) override {}
        void reset_articulation(ArticulationHandle handle) override;

        // Compile census, for diagnostics/tests: how many bodies and actuators the current world
        // model holds. Used to prove that spawn/remove cycles do not accumulate.
        int model_body_count() const;
        int model_actuator_count() const;

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
        float get_ground_plane_height() const override { return world_config.ground_plane_height; }

        // --- debug ---
        void collect_debug_lines(std::vector<DebugLine>& out_lines) const override;

    private:
        struct Articulation {
            bool valid = false;
            std::string root_link_name;            // the link that carries the free joint
            std::vector<std::string> link_names;   // index -> body name
            std::vector<std::string> joint_names;  // index -> joint name
            // Joint name -> index into joint_names/joint_descs/joint_commands. Built once at spawn so
            // command application is O(commands) instead of O(commands x joints) string compares.
            std::unordered_map<std::string, size_t> joint_index_by_name;
            std::vector<int> body_ids;             // index -> MuJoCo body id
            std::vector<int> joint_ids;            // index -> MuJoCo joint id
            std::vector<int> actuator_ids;         // index -> MuJoCo actuator id (-1 if free)
            // Per-joint controller configuration from the caller (kp/kv). The motor physics itself
            // (armature, friction loss, torque limit) lives in the cooked model, not here.
            std::vector<ArticulationJointDesc> joint_descs;
            std::vector<JointCommand> joint_commands;
        };

        // Spawn pose of an articulation, applied once at compile time so nothing has to move on the
        // first frames.
        struct SpawnPose {
            bud::math::vec3 position{ 0.0f };
            bud::math::quaternion rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            std::unordered_map<std::string, float> joint_angles;
        };

        // Compiles the accumulated spec into a model (deferred until the first step/query so the
        // scene and the robot can be added in any order).
        bool compile_world();
        void refresh_body_state();

        int find_body_id(const std::string& name) const;
        int find_joint_id(const std::string& name) const;
        int find_actuator_id(const std::string& name) const;
        void* body_user_data(int body_id) const;

        PhysicsWorldConfig world_config;
        mjSpec_* spec = nullptr;
        mjModel_* model = nullptr;
        mjData_* data = nullptr;
        // Kept opaque: MuJoCo's mjVFS is a typedef whose tag collides with a forward declaration.
        void* mesh_vfs = nullptr;
        // STL bytes served to MuJoCo through the VFS must outlive the model: mjVFS stores pointers.
        std::vector<std::vector<uint8_t>> mesh_buffers;
        // Mesh names already registered in the VFS. Respawning the same robot must not re-add them
        // (mj_addBufferVFS rejects a duplicate name), so registration is idempotent by model name.
        std::unordered_set<std::string> registered_mesh_names;
        bool spec_dirty = true;
        double accumulated_time = 0.0;

        RigidBodyStateSoA body_state;
        std::vector<int> handle_body_ids;          // our handle id -> MuJoCo body id
        std::vector<void*> handle_user_data;
        std::vector<bool> handle_static;
        // Static scene meshes are visual-only in this backend (no MuJoCo collision geometry is built
        // for them). Counted so the compile log states how much of the scene is non-colliding.
        int uncollidable_static_meshes = 0;
        std::vector<Articulation> articulations;
        std::vector<SpawnPose> spawn_poses;
        std::unordered_map<int, std::string> body_names_by_id;
        std::unordered_map<std::string, int> body_ids_by_name;
        std::unordered_map<std::string, int> joint_ids_by_name;
        std::unordered_map<std::string, int> actuator_ids_by_name;
        std::unordered_map<int, void*> user_data_by_body;

        ContactCallback contact_begin_cb;
        ContactCallback contact_persist_cb;
        ContactCallback contact_end_cb;
        std::vector<std::pair<int, int>> previous_contact_pairs;
        mutable std::recursive_mutex m_mutex;
    };

} // namespace bud::physics
