#pragma once

// Cooked MuJoCo model payload, stored in a robot .budasset as AssetChunkType::PhysicsModel.
//
// Robots are simulated by MuJoCo and MuJoCo is the only handler of their physics data (URDF
// semantics, inertials, joints, collision hulls, motors, contacts). The offline cook turns the URDF
// into normalized MJCF once and the runtime just compiles that - it never parses a raw URDF.
//
// Two things the cook must fix, both verified with the standalone spike:
//   1. The base joint. URDFs describe the base as "world --floating--> pelvis" and MuJoCo's
//      importer drops that joint, which welds the robot to the world (free joints = 0). Every
//      "it stands" result is then meaningless, so the pelvis gets an explicit free joint.
//   2. What the URDF cannot express: armature (rotor inertia), joint damping, Coulomb friction and
//      the per-joint torque motors. The cook bakes all of these into the MJCF (values copied from
//      Unitree's own MJCF), with the URDF effort limit as each motor's ctrlrange. Actuator gains
//      (kp/kd) are a controller choice and are deliberately NOT baked.
//      physics_metadata_json is a reserved slot for per-asset physics parameters the model format
//      cannot express; it is currently empty and no backend reads it.
//
// Meshes are referenced, not copied: the runtime rebuilds the STL bytes MuJoCo asks for from the
// existing visual .budasset RawMesh chunks (meshes[].asset_path), so a mesh used for both rendering
// and collision exists once in the package.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bud::robots {

    inline constexpr uint32_t MUJOCO_MODEL_CHUNK_VERSION = 1;

    // How the model payload is encoded. MJCF text is preferred (readable, version independent, and
    // lets meshes stay logical references to our own assets); MJB is the compiled binary and is the
    // fallback when MuJoCo's spec writer cannot produce XML. An MJB embeds the compiled collision
    // mesh data, so the mesh references below only matter for the visual bridge in that case.
    enum class MujocoModelFormat : uint32_t {
        Mjcf = 0,
        Mjb = 1
    };

    struct MujocoMeshRef {
        std::string mjcf_name;   // path as written in the MJCF, e.g. "meshes/pelvis.STL"
        std::string asset_path;  // content-relative asset providing it, e.g. "meshes/visual/pelvis.budasset"
    };

    struct MujocoModelData {
        uint32_t version = MUJOCO_MODEL_CHUNK_VERSION;
        MujocoModelFormat format = MujocoModelFormat::Mjcf;
        std::string model_payload;                       // MJCF text, or compiled MJB bytes
        std::vector<MujocoMeshRef> meshes;
        // Only rendering/animation consumes these (e.g. URDF limit.velocity, which the MuJoCo
        // importer deliberately drops, plus colours and the link -> visual mesh mapping).
        std::string render_metadata_json;
        // RESERVED slot, currently always empty and not read by any backend: physics parameters the
        // model format cannot express and that are not controller goals. Per-joint motor physics
        // (armature, friction loss, damping, torque limits) is baked into the MJCF payload instead.
        std::string physics_metadata_json;

        std::vector<uint8_t> serialize_binary() const;
        static std::optional<MujocoModelData> deserialize_binary(const uint8_t* data, size_t size);
        // Reads the PhysicsModel chunk out of a robot .budasset (same container walk as RobotDef).
        static std::optional<MujocoModelData> load_from_budasset(const std::string& path);
    };

} // namespace bud::robots
