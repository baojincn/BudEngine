#include "src/physics/bud.physics.world.mujoco.hpp"

#include "src/core/bud.asset.types.hpp"
#include "src/core/bud.logger.hpp"
#include "src/core/bud.raw_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>
#include <mujoco/mujoco.h>

namespace bud::physics {

    namespace {

#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
        mjSpec* safe_parse_xml(const char* xml, const mjVFS* vfs, char* error, int error_sz) {
            __try {
                return mj_parseXMLString(xml, vfs, error, error_sz);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (error && error_sz > 0) {
                    snprintf(error, error_sz, "SEH Exception in mj_parseXMLString (code: 0x%08lX)", (unsigned long)GetExceptionCode());
                }
                return nullptr;
            }
        }
        mjsElement* safe_attach(mjsElement* parent, const mjsElement* child, const char* prefix, const char* suffix, char* error, int error_sz) {
            __try {
                return mjs_attach(parent, child, prefix, suffix);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (error && error_sz > 0) {
                    snprintf(error, error_sz, "SEH Exception in mjs_attach (code: 0x%08lX)", (unsigned long)GetExceptionCode());
                }
                return nullptr;
            }
        }
#else
        mjSpec* safe_parse_xml(const char* xml, const mjVFS* vfs, char* error, int error_sz) {
            return mj_parseXMLString(xml, vfs, error, error_sz);
        }
        mjsElement* safe_attach(mjsElement* parent, const mjsElement* child, const char* prefix, const char* suffix, char* error, int error_sz) {
            return mjs_attach(parent, child, prefix, suffix);
        }
#endif

        // Reads one chunk out of a .budasset container. The backend reads assets synchronously and
        // directly: the package format is a flat header plus a chunk table, and the async asset
        // manager belongs to the engine's streaming concerns, not to the solver.
        std::vector<char> read_asset_chunk(const std::string& path, bud::asset::AssetChunkType type) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return {};

            bud::asset::BudAssetHeader header{};
            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            if (!in)
                return {};

            in.seekg(static_cast<std::streamoff>(header.chunk_table_offset), std::ios::beg);
            for (uint32_t i = 0; i < header.chunk_count; ++i) {
                bud::asset::AssetChunkEntry entry{};
                in.read(reinterpret_cast<char*>(&entry), sizeof(entry));
                if (!in)
                    return {};
                if (entry.chunk_type != static_cast<uint32_t>(type))
                    continue;

                std::vector<char> data(entry.size);
                in.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
                in.read(data.data(), static_cast<std::streamsize>(data.size()));
                if (!in)
                    return {};
                return data;
            }
            return {};
        }

        // MuJoCo loads meshes from STL/OBJ files. Our meshes live as RawMesh chunks inside the mesh
        // .budasset, so the bytes MuJoCo asks for are rebuilt here - the package stores each mesh
        // once and both rendering and collision read it.
        std::vector<uint8_t> raw_mesh_chunk_to_stl(const std::vector<char>& raw_mesh_bytes) {
            if (raw_mesh_bytes.empty())
                return {};

            auto raw = bud::asset::RawMesh::deserialize_binary(
                reinterpret_cast<const uint8_t*>(raw_mesh_bytes.data()), raw_mesh_bytes.size());
            if (!raw)
                return {};

            const size_t triangle_count = raw->indices.size() / 3;
            std::vector<uint8_t> stl;
            stl.resize(84 + triangle_count * 50);
            std::memcpy(stl.data(), "BudEngine RawMesh -> STL", 24);
            const uint32_t count = static_cast<uint32_t>(triangle_count);
            std::memcpy(stl.data() + 80, &count, 4);

            uint8_t* cursor = stl.data() + 84;
            const auto write_float = [](uint8_t* dst, float value) {
                std::memcpy(dst, &value, 4);
            };
            for (size_t triangle = 0; triangle < triangle_count; ++triangle) {
                const bud::asset::RawVertex& a = raw->vertices[raw->indices[triangle * 3 + 0]];
                const bud::asset::RawVertex& b = raw->vertices[raw->indices[triangle * 3 + 1]];
                const bud::asset::RawVertex& c = raw->vertices[raw->indices[triangle * 3 + 2]];
                // Normal is optional in STL; writing zeros lets MuJoCo compute it.
                for (int i = 0; i < 3; ++i)
                    write_float(cursor + i * 4, 0.0f);
                for (int i = 0; i < 3; ++i) {
                    write_float(cursor + 12 + i * 4, a.position[i]);
                    write_float(cursor + 24 + i * 4, b.position[i]);
                    write_float(cursor + 36 + i * 4, c.position[i]);
                }
                cursor[48] = 0;
                cursor[49] = 0;
                cursor += 50;
            }
            return stl;
        }

        mjtGeom shape_to_geom(bud::physics::ShapeType type) {
            switch (type) {
            case bud::physics::ShapeType::Sphere: return mjGEOM_SPHERE;
            case bud::physics::ShapeType::Capsule: return mjGEOM_CAPSULE;
            case bud::physics::ShapeType::Plane: return mjGEOM_PLANE;
            case bud::physics::ShapeType::Box:
            default: return mjGEOM_BOX;
            }
        }

        // ------------------------------------------------------------------
        // Coordinate convention.
        // ------------------------------------------------------------------
        // MuJoCo is Z-up, the engine is Y-up. Every value crossing this boundary is converted here
        // so callers keep working in engine coordinates. The basis change is a +90 degree rotation
        // about X: engine (x, y, z) -> MuJoCo (x, -z, y), and back (x, y, z) -> (x, z, -y).
        //
        // Getting this wrong is not subtle: feeding engine gravity straight in made robots fall
        // sideways in MuJoCo's -Y and never touch the floor.
        bud::math::vec3 to_mujoco(const bud::math::vec3& v) {
            return bud::math::vec3(v.x, -v.z, v.y);
        }

        bud::math::vec3 from_mujoco(const bud::math::vec3& v) {
            return bud::math::vec3(v.x, v.z, -v.y);
        }

        // The same basis change applied to directions (no translation component).
        bud::math::vec3 direction_to_mujoco(const bud::math::vec3& v) {
            return to_mujoco(v);
        }

        bud::math::vec3 direction_from_mujoco(const bud::math::vec3& v) {
            return from_mujoco(v);
        }

        // q_B is the quaternion of that +90 degree X rotation (w, x, y, z).
        constexpr double kSqrtHalf = 0.7071067811865476;

        bud::math::quaternion to_mujoco(const bud::math::quaternion& q) {
            // q_mj = q_B * q_eng * q_B^-1
            const bud::math::quaternion q_b(static_cast<float>(kSqrtHalf), static_cast<float>(kSqrtHalf),
                                            0.0f, 0.0f);
            const bud::math::quaternion q_b_inv(static_cast<float>(kSqrtHalf),
                                                static_cast<float>(-kSqrtHalf), 0.0f, 0.0f);
            return glm::normalize(q_b * q * q_b_inv);
        }

        bud::math::quaternion from_mujoco(const bud::math::quaternion& q) {
            const bud::math::quaternion q_b_inv(static_cast<float>(kSqrtHalf),
                                                static_cast<float>(-kSqrtHalf), 0.0f, 0.0f);
            const bud::math::quaternion q_b(static_cast<float>(kSqrtHalf), static_cast<float>(kSqrtHalf),
                                            0.0f, 0.0f);
            return glm::normalize(q_b_inv * q * q_b);
        }

        // MuJoCo's own defaults are: timestep 0.002 s, iterations 100, solver NEWTON, integrator
        // IMPLICITFAST. Solver and integrator stay at those defaults, which is also what the official
        // Unitree setup runs (their MJCF does not override them).
        //
        // timestep: pinned explicitly at 0.002 s (500 Hz). Unitree's own simulator runs
        //   SIMULATE_DT = 0.003 s (333 Hz); the finer step keeps the motor PD well behaved at the leg
        //   gains we use, and a single robot costs little, so there is no reason to coarsen it.
        // iterations: kept at MuJoCo's default (100) rather than an arbitrary lower value, so the
        //   solver work matches the reference setup.
        constexpr double k_solver_timestep = 0.002;
        constexpr int k_solver_iterations = 100;

    } // namespace

    MujocoPhysicsWorld::MujocoPhysicsWorld() = default;

    MujocoPhysicsWorld::~MujocoPhysicsWorld() {
        if (data)
            mj_deleteData(data);
        if (model)
            mj_deleteModel(model);
        if (spec)
            mj_deleteSpec(spec);
        if (mesh_vfs) {
            mj_deleteVFS(static_cast<mjVFS*>(mesh_vfs));
            delete static_cast<mjVFS*>(mesh_vfs);
        }
    }

    bool MujocoPhysicsWorld::init(const PhysicsWorldConfig& config) {
        mju_user_error = [](const char* msg) {
            std::cerr << "[MuJoCo Error Handler] " << msg << std::endl;
            bud::eprint("[MuJoCo Error Handler] {}", msg);
            throw std::runtime_error(std::string("MuJoCo Error: ") + msg);
        };
        mju_user_warning = [](const char* msg) {
            std::cout << "[MuJoCo Warning Handler] " << msg << std::endl;
            bud::print("[MuJoCo Warning Handler] {}", msg);
        };

        world_config = config;
        spec = mj_makeSpec();
        if (!spec) {
            bud::eprint("[MuJoCo] mj_makeSpec failed");
            return false;
        }

        mjVFS* vfs = new mjVFS();
        mj_defaultVFS(vfs);
        mesh_vfs = vfs;

        body_state.resize(config.max_bodies);
        handle_body_ids.clear();
        handle_body_ids.reserve(config.max_bodies);
        handle_body_names.clear();
        handle_body_names.reserve(config.max_bodies);
        handle_user_data.clear();
        handle_user_data.reserve(config.max_bodies);
        handle_static.clear();
        handle_static.reserve(config.max_bodies);
        handle_half_extents.clear();
        handle_half_extents.reserve(config.max_bodies);
        uncollidable_static_meshes = 0;
        proxy_box_static_colliders = 0;

        if (config.enable_ground_plane) {
            mjsBody* world_body = mjs_findBody(spec, "world");
            if (world_body) {
                mjsGeom* ground = mjs_addGeom(world_body, nullptr);
                mjs_setName(ground->element, "ground_plane");
                ground->type = mjGEOM_PLANE;
                const bud::math::vec3 ground_pos = to_mujoco(bud::math::vec3(0.0f, config.ground_plane_height, 0.0f));
                ground->pos[0] = ground_pos.x;
                ground->pos[1] = ground_pos.y;
                ground->pos[2] = ground_pos.z;
                constexpr double kPlaneHalfExtent = 50.0;
                constexpr double kPlaneGridSpacing = 0.1;
        // Torsional friction has to be high enough that a planted foot does not twist freely: with a
        // near-zero value a walking policy veers off its commanded heading even though it steps.
        constexpr double kTorsionalFriction = 0.5;
        constexpr double kRollingFriction = 0.001;
                ground->size[0] = kPlaneHalfExtent;
                ground->size[1] = kPlaneHalfExtent;
                ground->size[2] = kPlaneGridSpacing;
                ground->friction[0] = config.ground_friction;
                ground->friction[1] = kTorsionalFriction;
                ground->friction[2] = kRollingFriction;
                // The floor must collide with every robot, and cooked models do not agree on which
                // contact bit they use (G1's URDF geoms use bit 1, the Microduck MJCF uses bit 2).
                // A narrow 1/1 mask silently makes the duck fall through the ground, so the plane
                // advertises every bit in conaffinity.
                ground->contype = 1;
                ground->conaffinity = 0xFFFF;
                ground->condim = 4;
                ground->solref[0] = 0.004;
                ground->solref[1] = 1.0;
                ground->solimp[0] = 0.9;
                ground->solimp[1] = 0.95;
                ground->solimp[2] = 0.001;
                ground->solimp[3] = 0.5;
                ground->solimp[4] = 2.0;
                bud::print("[MuJoCo] ground plane created at y={:.3f} (friction={:.2f}, condim=4)",
                           config.ground_plane_height, config.ground_friction);

                // Add 4 courtyard perimeter boundary walls (North, South, East, West)
                // to enclose the walkable Sponza courtyard and prevent robot falling into the void.
                struct BoundaryWall {
                    const char* name;
                    bud::math::vec3 center;
                    bud::math::vec3 half_extent;
                };
                const BoundaryWall kPerimeterWalls[] = {
                    { "perimeter_wall_north", { 0.0f, 1.75f, 6.3f },  { 14.5f, 1.75f, 0.3f } },
                    { "perimeter_wall_south", { 0.0f, 1.75f, -6.3f }, { 14.5f, 1.75f, 0.3f } },
                    { "perimeter_wall_east",  { 14.3f, 1.75f, 0.0f },  { 0.3f, 1.75f, 6.5f } },
                    { "perimeter_wall_west",  { -14.3f, 1.75f, 0.0f }, { 0.3f, 1.75f, 6.5f } },
                };
                for (const auto& w : kPerimeterWalls) {
                    mjsBody* wall_body = mjs_addBody(world_body, nullptr);
                    mjs_setName(wall_body->element, w.name);
                    const bud::math::vec3 mj_pos = to_mujoco(w.center);
                    wall_body->pos[0] = mj_pos.x;
                    wall_body->pos[1] = mj_pos.y;
                    wall_body->pos[2] = mj_pos.z;

                    mjsGeom* wall_geom = mjs_addGeom(wall_body, nullptr);
                    mjs_setName(wall_geom->element, (std::string(w.name) + "_geom").c_str());
                    wall_geom->type = mjGEOM_BOX;
                    wall_geom->size[0] = std::abs(w.half_extent.x);
                    wall_geom->size[1] = std::abs(w.half_extent.z);
                    wall_geom->size[2] = std::abs(w.half_extent.y);
                    wall_geom->friction[0] = 0.8;
                    wall_geom->friction[1] = 0.8;
                    // Same reasoning as the floor: collide with any robot regardless of which contact
                    // bit its cooked model uses.
                    wall_geom->contype = 1;
                    wall_geom->conaffinity = 0xFFFF;
                    wall_geom->condim = 3;

                    handle_body_ids.push_back(-1);
                    handle_body_names.push_back(w.name);
                    handle_user_data.push_back(nullptr);
                    handle_static.push_back(true);
                    handle_half_extents.push_back(w.half_extent);
                    ++proxy_box_static_colliders;
                }
                bud::print("[MuJoCo] created 4 courtyard perimeter boundary walls");
            }
        }

        spec_dirty = true;
        bud::print("[MuJoCo] world initialised (bodies are compiled on the first step)");
        return true;
    }

    RigidBodyHandle MujocoPhysicsWorld::add_rigid_body(const RigidBodyDesc& desc) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return {};
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        mjsBody* world_body = mjs_findBody(spec, "world");
        if (!world_body) {
            bud::eprint("[MuJoCo] world body missing from the spec");
            return {};
        }

        if (desc.shape.type == ShapeType::Mesh || desc.shape.type == ShapeType::ConvexHull) {
            if (!desc.shape.vertices.empty()) {
                bud::math::vec3 min_pt(1.0e9f);
                bud::math::vec3 max_pt(-1.0e9f);
                for (const auto& v : desc.shape.vertices) {
                    min_pt = glm::min(min_pt, v);
                    max_pt = glm::max(max_pt, v);
                }
                const bud::math::vec3 size = max_pt - min_pt;
                const bud::math::vec3 center = (min_pt + max_pt) * 0.5f;
                const bud::math::vec3 half_extent = size * 0.5f;

                // 1. Skip large floor slabs (handled cleanly by mathematical ground plane mjGEOM_PLANE)
                const bool is_floor = (max_pt.y <= 0.15f && (size.x > 5.0f || size.z > 5.0f));

                // 2. Skip upper-level structures above robot reachable height (roofs, 2nd floor, ceilings)
                const bool is_upper = (min_pt.y > 2.0f);

                // 3. Skip giant enclosing backdrops / lids to prevent hollow spaces becoming solid blocks
                const bool is_enclosing = (size.x > 18.0f && size.z > 10.0f);

                // 4. Ground obstacles: courtyard pillars, ground walls, ground plinths/props
                const bool is_ground_obstacle = (!is_floor && !is_upper && !is_enclosing &&
                                                 min_pt.y <= 1.8f && max_pt.y >= 0.05f &&
                                                 half_extent.x > 0.01f && half_extent.y > 0.01f && half_extent.z > 0.01f);

                if (is_ground_obstacle) {
                    mjsBody* body = mjs_addBody(world_body, nullptr);
                    const std::string body_name = "proxy_box_" + std::to_string(handle_body_ids.size());
                    mjs_setName(body->element, body_name.c_str());

                    const bud::math::vec3 mujoco_position = to_mujoco(center);
                    body->pos[0] = mujoco_position.x;
                    body->pos[1] = mujoco_position.y;
                    body->pos[2] = mujoco_position.z;

                    mjsGeom* geom = mjs_addGeom(body, nullptr);
                    mjs_setName(geom->element, (body_name + "_geom").c_str());
                    geom->type = mjGEOM_BOX;
                    geom->size[0] = std::abs(half_extent.x);
                    geom->size[1] = std::abs(half_extent.z);
                    geom->size[2] = std::abs(half_extent.y);

                    constexpr double kDefaultProxyFriction = 0.8;
                    geom->friction[0] = desc.material.friction > 0.0f ? desc.material.friction : kDefaultProxyFriction;
                    geom->friction[1] = desc.material.friction > 0.0f ? desc.material.friction : kDefaultProxyFriction;
                    geom->contype = 1;
                    geom->conaffinity = 1;
                    geom->condim = 3;

                    const uint32_t handle_id = static_cast<uint32_t>(handle_body_ids.size());
                    handle_body_ids.push_back(-1); // resolved at compile time
                    handle_body_names.push_back(body_name);
                    handle_user_data.push_back(nullptr);
                    handle_static.push_back(true);
                    handle_half_extents.push_back(half_extent);
                    spec_dirty = true;
                    ++proxy_box_static_colliders;
                    return RigidBodyHandle{ handle_id };
                }
            }

            // Non-obstacle meshes (floors, roofs, high arches) remain visual-only
            ++uncollidable_static_meshes;
            const uint32_t handle_id = static_cast<uint32_t>(handle_body_ids.size());
            handle_body_ids.push_back(-1);
            handle_body_names.push_back("");
            handle_user_data.push_back(nullptr);
            handle_static.push_back(true);
            handle_half_extents.push_back(bud::math::vec3(0.0f));
            return RigidBodyHandle{ handle_id };
        }

        mjsBody* body = mjs_addBody(world_body, nullptr);
        const std::string body_name = "body_" + std::to_string(handle_body_ids.size());
        mjs_setName(body->element, body_name.c_str());
        const bud::math::vec3 mujoco_position = to_mujoco(desc.position);
        const bud::math::quaternion mujoco_rotation = to_mujoco(desc.rotation);
        body->pos[0] = mujoco_position.x;
        body->pos[1] = mujoco_position.y;
        body->pos[2] = mujoco_position.z;
        body->quat[0] = mujoco_rotation.w;
        body->quat[1] = mujoco_rotation.x;
        body->quat[2] = mujoco_rotation.y;
        body->quat[3] = mujoco_rotation.z;

        if (desc.motion_type == MotionType::Dynamic)
            mjs_addFreeJoint(body);

        mjsGeom* geom = mjs_addGeom(body, nullptr);
        mjs_setName(geom->element, (body_name + "_geom").c_str());
        geom->type = shape_to_geom(desc.shape.type);
        switch (desc.shape.type) {
        case ShapeType::Sphere:
            geom->size[0] = desc.shape.radius;
            break;
        case ShapeType::Capsule:
            geom->size[0] = desc.shape.capsule_radius;
            geom->size[1] = desc.shape.capsule_half_height;
            break;
        case ShapeType::Plane: {
            constexpr float kDefaultPlaneHalfExtent = 50.0f;
            constexpr float kDefaultPlaneSpacing = 0.1f;
            geom->size[0] = std::abs(desc.shape.half_extent.x) > 0.0f ? std::abs(desc.shape.half_extent.x) : kDefaultPlaneHalfExtent;
            geom->size[1] = std::abs(desc.shape.half_extent.z) > 0.0f ? std::abs(desc.shape.half_extent.z) : kDefaultPlaneHalfExtent;
            geom->size[2] = kDefaultPlaneSpacing;
            break;
        }
        default:
            // Half extents are magnitudes in the body frame: permute the axes for the basis change
            // without the sign flip (a negative half extent fails the compile).
            geom->size[0] = std::abs(desc.shape.half_extent.x);
            geom->size[1] = std::abs(desc.shape.half_extent.z);
            geom->size[2] = std::abs(desc.shape.half_extent.y);
            break;
        }
        geom->friction[0] = desc.material.friction;
        geom->friction[1] = desc.material.friction;
        // Collision masks: geoms created through the spec API start with contype/conaffinity 0
        // (unlike MJCF, whose compiler default is 1), i.e. they never collide. Set them explicitly.
        geom->contype = 1;
        geom->conaffinity = 1;
        geom->condim = 3;

        const uint32_t handle_id = static_cast<uint32_t>(handle_body_ids.size());
        handle_body_ids.push_back(-1); // resolved at compile time
        handle_body_names.push_back(body_name);
        handle_user_data.push_back(nullptr);
        handle_static.push_back(desc.motion_type != MotionType::Dynamic);
        handle_half_extents.push_back(desc.shape.half_extent);
        spec_dirty = true;
        return RigidBodyHandle{ handle_id };
    }

    void MujocoPhysicsWorld::remove_rigid_body(RigidBodyHandle) {
        // Rebuilding the spec would reset the simulation; removal is not needed by the robot flow
        // yet, so it is reported instead of silently invalidating handles.
        bud::eprint("[MuJoCo] remove_rigid_body is not implemented yet");
    }

    ArticulationHandle MujocoPhysicsWorld::create_articulation(const ArticulationDesc& desc) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return {};
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!desc.cooked_model.is_valid()) {
            bud::eprint("{}", "[MuJoCo] articulated body '" + desc.name + "' has no cooked model payload");
            return {};
        }
        if (desc.cooked_model.format != CookedModelFormat::MjcfText) {
            // MJB would have to be merged as a compiled model; the cook emits MJCF (the preferred
            // format because meshes stay logical references), so this is a guard rail.
            bud::eprint("{}", "[MuJoCo] only MJCF cooked models are supported for now ('" + desc.name + "')");
            return {};
        }

        // Serve the meshes the model asks for out of our own assets. Failures are reported per mesh
        // (they mean the robot will not compile), but the happy path stays to a single summary line.
        size_t mesh_bytes_total = 0;
        int mesh_buffers_added = 0;
        int mesh_buffers_reused = 0;
        for (const auto& mesh : desc.cooked_model.meshes) {
            // A second articulation of the same robot reuses the VFS entries already registered.
            if (registered_mesh_names.count(mesh.model_name) > 0) {
                ++mesh_buffers_reused;
                continue;
            }
            // Path resolution is backend-agnostic: the loader hands us a path that either exists
            // as-is or is relative to the configured asset root. No robot-specific fallbacks.
            std::filesystem::path asset_path = mesh.asset_path;
            if (!std::filesystem::exists(asset_path))
                asset_path = std::filesystem::path(world_config.asset_root) / mesh.asset_path;
            const std::vector<char> chunk = read_asset_chunk(asset_path.string(), bud::asset::AssetChunkType::RawMesh);
            if (chunk.empty()) {
                bud::eprint("{}", "[MuJoCo] mesh asset missing or has no RawMesh chunk: " + asset_path.string());
                continue;
            }
            mesh_buffers.push_back(raw_mesh_chunk_to_stl(chunk));
            const std::vector<uint8_t>& stl = mesh_buffers.back();
            if (stl.empty()) {
                bud::eprint("{}", "[MuJoCo] failed to convert mesh to STL: " + mesh.asset_path);
                continue;
            }
            const int add_result =
                mj_addBufferVFS(static_cast<mjVFS*>(mesh_vfs), mesh.model_name.c_str(), stl.data(),
                                static_cast<int>(stl.size()));
            if (add_result != 0) {
                bud::eprint("{}", "[MuJoCo] mj_addBufferVFS failed for mesh '" + mesh.model_name + "'");
                continue;
            }
            mesh_bytes_total += stl.size();
            registered_mesh_names.insert(mesh.model_name);
            ++mesh_buffers_added;
        }

        char error[2048] = { 0 };
        const std::string mjcf(reinterpret_cast<const char*>(desc.cooked_model.payload.data()),
                               desc.cooked_model.payload.size());
        mjSpec* robot_spec = safe_parse_xml(mjcf.c_str(), static_cast<mjVFS*>(mesh_vfs), error, sizeof(error));
        if (!robot_spec) {
            bud::eprint("{}", std::string("[MuJoCo] parsing the cooked model failed: ") + error);
            return {};
        }

        bud::print("[MuJoCo] robot '{}' model parsed: {} meshes added ({} bytes), {} reused from the "
                   "VFS, MJCF {} bytes",
                   desc.name, mesh_buffers_added, mesh_bytes_total, mesh_buffers_reused, mjcf.size());
        mjsBody* world_body = mjs_findBody(spec, "world");
        mjsBody* robot_root = mjs_findBody(robot_spec, desc.root_link.c_str());
        if (!world_body || !robot_root) {
            bud::eprint("{}", "[MuJoCo] could not find the world body or the robot root link '" + desc.root_link + "'");
            mj_deleteSpec(robot_spec);
            return {};
        }

        // Re-attaching a robot whose meshes are already in the world would fail with
        // "repeated name ... in mesh": the parent spec still holds them from the previous attach.
        // Drop the duplicates from the child spec so the attach reuses the parent's assets.
        {
            std::vector<mjsElement*> duplicate_meshes;
            for (mjsElement* element = mjs_firstElement(robot_spec, mjOBJ_MESH); element != nullptr;
                 element = mjs_nextElement(robot_spec, element)) {
                mjString* name = mjs_getName(element);
                if (!name)
                    continue;
                const char* mesh_name = mjs_getString(name);
                if (mesh_name != nullptr && mjs_findElement(spec, mjOBJ_MESH, mesh_name) != nullptr)
                    duplicate_meshes.push_back(element);
            }
            for (mjsElement* element : duplicate_meshes)
                mjs_delete(robot_spec, element);

            // Avoid site name collisions in multi-articulation scenes
            for (mjsElement* element = mjs_firstElement(robot_spec, mjOBJ_SITE); element != nullptr;
                 element = mjs_nextElement(robot_spec, element)) {
                mjString* name = mjs_getName(element);
                if (!name)
                    continue;
                const char* site_name = mjs_getString(name);
                if (site_name != nullptr && mjs_findElement(spec, mjOBJ_SITE, site_name) != nullptr) {
                    std::string new_name = desc.name + "_" + site_name;
                    mjs_setName(element, new_name.c_str());
                }
            }

            // Avoid sensor name collisions in multi-articulation scenes
            for (mjsElement* element = mjs_firstElement(robot_spec, mjOBJ_SENSOR); element != nullptr;
                 element = mjs_nextElement(robot_spec, element)) {
                mjString* name = mjs_getName(element);
                if (!name)
                    continue;
                const char* sensor_name = mjs_getString(name);
                if (sensor_name != nullptr && mjs_findElement(spec, mjOBJ_SENSOR, sensor_name) != nullptr) {
                    std::string new_name = desc.name + "_" + sensor_name;
                    mjs_setName(element, new_name.c_str());
                }
            }
        }

        // Add collision geometry for Microduck so it collides with G1, environment, and floor
        if (desc.name == "microduck" || desc.name.find("duck") != std::string::npos) {
            // Head collision sphere: enables physical contact against G1 and prevents ground penetration
            if (mjsBody* head_body = mjs_findBody(robot_spec, "jaw_soft")) {
                mjsGeom* geom = mjs_addGeom(head_body, nullptr);
                mjs_setName(geom->element, "duck_head_col");
                geom->type = mjGEOM_SPHERE;
                geom->size[0] = 0.052; // 5.2cm radius
                geom->pos[0] = 0.005;
                geom->pos[1] = 0.0;
                geom->pos[2] = 0.015;
                geom->contype = 1;
                geom->conaffinity = 1;
                geom->condim = 3;
                geom->friction[0] = 0.8;
                geom->friction[1] = 0.8;
                geom->friction[2] = 0.001;
            }

            // Trunk collision box: covers body & battery against G1 and prevents falling through ground
            if (mjsBody* trunk_body = mjs_findBody(robot_spec, "trunk_base")) {
                mjsGeom* geom = mjs_addGeom(trunk_body, nullptr);
                mjs_setName(geom->element, "duck_trunk_col");
                geom->type = mjGEOM_BOX;
                geom->size[0] = 0.045; // length half-extent (9cm total)
                geom->size[1] = 0.032; // width half-extent (6.4cm total, fits inside leg clearance)
                geom->size[2] = 0.030; // height half-extent (6cm total)
                geom->pos[0] = 0.005;
                geom->pos[1] = 0.0;
                geom->pos[2] = -0.005;
                geom->contype = 1;
                geom->conaffinity = 1;
                geom->condim = 3;
                geom->friction[0] = 0.8;
                geom->friction[1] = 0.8;
                geom->friction[2] = 0.001;
            }

            // Enable world & robot collision for self_collision_only geoms (legs/shins)
            for (mjsElement* element = mjs_firstElement(robot_spec, mjOBJ_GEOM); element != nullptr;
                 element = mjs_nextElement(robot_spec, element)) {
                if (mjsGeom* geom = mjs_asGeom(element)) {
                    if (geom->contype == 2 || geom->conaffinity == 2) {
                        geom->contype |= 1;
                        geom->conaffinity |= 1;
                    }
                }
            }
            bud::print("[MuJoCo] Added collision primitives for Microduck head & trunk, enabled world collision on leg geoms");
        }

        // Attach the whole robot spec into our world. Passing the spec's own element (elemtype
        // mjOBJ_MODEL) is what mjs_attach is built for: it wires the child's worldbody through a
        // frame and moves its children under ours, free joint included.
        char attach_seh_error[512] = { 0 };
        mjsElement* attached = nullptr;
        try {
            attached = safe_attach(world_body->element, robot_spec->element, "", "", attach_seh_error, sizeof(attach_seh_error));
        } catch (const std::exception& e) {
            bud::eprint("[MuJoCo] mjs_attach exception: {}", e.what());
            std::cerr << "[MuJoCo] mjs_attach exception: " << e.what() << std::endl;
            mj_deleteSpec(robot_spec);
            return {};
        }

        if (attach_seh_error[0] != '\0') {
            bud::eprint("[MuJoCo] safe_attach SEH error: {}", attach_seh_error);
            std::cerr << "[MuJoCo] safe_attach SEH error: " << attach_seh_error << std::endl;
        }

        mj_deleteSpec(robot_spec);
        bud::print("[MuJoCo] robot attach {}", attached ? "ok" : "failed");
        if (!attached) {
            const char* attach_error = mjs_getError(spec);
            bud::eprint("{}", std::string("[MuJoCo] attaching the robot to the world failed: ") +
                                  (attach_error ? attach_error : "(no message)"));
            return {};
        }

        Articulation articulation;
        articulation.valid = true;
        articulation.name = desc.name;
        articulation.root_link_name = desc.root_link;
        for (const auto& link : desc.links)
            articulation.link_names.push_back(link.name);
        articulation.joint_index_by_name.reserve(desc.joints.size());
        for (const auto& joint : desc.joints) {
            articulation.joint_index_by_name[joint.name] = articulation.joint_names.size();
            articulation.joint_names.push_back(joint.name);
            articulation.joint_descs.push_back(joint);
        }
        articulation.body_ids.assign(articulation.link_names.size(), -1);
        articulation.joint_ids.assign(articulation.joint_names.size(), -1);
        articulation.actuator_ids.assign(articulation.joint_names.size(), -1);
        articulation.joint_commands.resize(articulation.joint_names.size());
        for (size_t i = 0; i < articulation.joint_names.size(); ++i) {
            articulation.joint_commands[i].joint_name = articulation.joint_names[i];
            articulation.joint_commands[i].kp = articulation.joint_descs[i].stiffness > 0.0f
                                                    ? articulation.joint_descs[i].stiffness
                                                    : 40.0f;
            articulation.joint_commands[i].kd = articulation.joint_descs[i].damping > 0.0f
                                                    ? articulation.joint_descs[i].damping
                                                    : 2.0f;
            const auto it = desc.initial_joint_angles.find(articulation.joint_names[i]);
            if (it != desc.initial_joint_angles.end())
                articulation.joint_commands[i].q = it->second;
        }
        articulations.push_back(std::move(articulation));

        spawn_poses.push_back({ desc.root_position, desc.root_rotation, desc.initial_joint_angles });
        spec_dirty = true;

        const uint32_t index = static_cast<uint32_t>(articulations.size() - 1);
        bud::print("{}", "[MuJoCo] articulation '" + desc.name + "' attached (" +
                   std::to_string(desc.links.size()) + " links, " +
                   std::to_string(desc.joints.size()) + " joints)");
        return ArticulationHandle{ index };
    }

    void MujocoPhysicsWorld::remove_articulation(ArticulationHandle handle) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!spec || !handle.is_valid() || handle.id >= articulations.size())
            return;
        Articulation& articulation = articulations[handle.id];
        if (!articulation.valid)
            return;

        // Remove the motors that drive this articulation's joints first. Matching by target joint
        // (not by actuator name) keeps this correct whatever the cook names its motors, and it works
        // whether or not the world has been compiled yet.
        const std::unordered_set<std::string> joint_names(articulation.joint_names.begin(),
                                                          articulation.joint_names.end());
        const std::unordered_set<std::string> link_names(articulation.link_names.begin(),
                                                         articulation.link_names.end());
        std::vector<mjsElement*> actuators_to_delete;
        for (mjsElement* element = mjs_firstElement(spec, mjOBJ_ACTUATOR); element != nullptr;
             element = mjs_nextElement(spec, element)) {
            mjsActuator* actuator = mjs_asActuator(element);
            if (!actuator || actuator->trntype != mjTRN_JOINT || actuator->target == nullptr)
                continue;
            const char* target = mjs_getString(actuator->target);
            if (target != nullptr && joint_names.count(target) > 0)
                actuators_to_delete.push_back(element);
        }
        for (mjsElement* element : actuators_to_delete)
            mjs_delete(spec, element);

        // Sensors that reference sites or bodies inside this articulation must go too: deleting the
        // body subtree does not remove them, and a respawn would then collide on their names.
        std::vector<mjsElement*> sensors_to_delete;
        for (mjsElement* element = mjs_firstElement(spec, mjOBJ_SENSOR); element != nullptr;
             element = mjs_nextElement(spec, element)) {
            mjsSensor* sensor = mjs_asSensor(element);
            if (sensor == nullptr || sensor->objname == nullptr)
                continue;
            mjsElement* referenced = nullptr;
            if (sensor->objtype == mjOBJ_SITE)
                referenced = mjs_findElement(spec, mjOBJ_SITE, mjs_getString(sensor->objname));
            else if (sensor->objtype == mjOBJ_BODY)
                referenced = mjs_findElement(spec, mjOBJ_BODY, mjs_getString(sensor->objname));
            if (referenced == nullptr)
                continue;
            mjsElement* owner = referenced;
            if (sensor->objtype == mjOBJ_SITE) {
                mjsBody* parent_body = mjs_getParent(referenced);
                owner = (parent_body != nullptr) ? parent_body->element : nullptr;
            }
            if (owner == nullptr)
                continue;
            const char* owner_name = mjs_getString(mjs_getName(owner));
            if (owner_name != nullptr && link_names.count(owner_name) > 0)
                sensors_to_delete.push_back(element);
        }
        for (mjsElement* element : sensors_to_delete)
            mjs_delete(spec, element);

        // Delete the body subtree. mjs_delete cascades into the child bodies, geoms and joints of
        // this articulation, so nothing accumulates across spawn/remove cycles.
        const std::string& root_name = articulation.root_link_name;
        if (!root_name.empty()) {
            mjsElement* root_body = mjs_findElement(spec, mjOBJ_BODY, root_name.c_str());
            if (root_body)
                mjs_delete(spec, root_body);
        }

        articulation.valid = false;
        articulation.body_ids.assign(articulation.body_ids.size(), -1);
        articulation.joint_ids.assign(articulation.joint_ids.size(), -1);
        articulation.actuator_ids.assign(articulation.actuator_ids.size(), -1);
        // The world has to be rebuilt without this robot. This is a structural change: the next
        // compile resets the remaining articulations' state, which is why reset_articulation()
        // deliberately does not go through here.
        spec_dirty = true;
        bud::print("[MuJoCo] articulation '{}' removed", root_name.empty() ? "(unnamed)" : root_name);
    }

    void MujocoPhysicsWorld::reset_articulation(ArticulationHandle handle) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !model || !data || !handle.is_valid() ||
            handle.id >= articulations.size())
            return;
        Articulation& articulation = articulations[handle.id];
        if (!articulation.valid || handle.id >= spawn_poses.size())
            return;
        const SpawnPose& pose = spawn_poses[handle.id];

        // Root transform: the articulation's root link carries the free joint.
        if (!articulation.body_ids.empty() && articulation.body_ids[0] >= 0) {
            const int root_body = articulation.body_ids[0];
            const int joint = model->body_jntadr[root_body];
            if (joint >= 0 && model->jnt_type[joint] == mjJNT_FREE) {
                const int qpos_adr = model->jnt_qposadr[joint];
                const int dof_adr = model->jnt_dofadr[joint];
                const bud::math::vec3 position = to_mujoco(pose.position);
                const bud::math::quaternion rotation = to_mujoco(pose.rotation);
                data->qpos[qpos_adr + 0] = position.x;
                data->qpos[qpos_adr + 1] = position.y;
                data->qpos[qpos_adr + 2] = position.z;
                data->qpos[qpos_adr + 3] = rotation.w;
                data->qpos[qpos_adr + 4] = rotation.x;
                data->qpos[qpos_adr + 5] = rotation.y;
                data->qpos[qpos_adr + 6] = rotation.z;
                for (int k = 0; k < 6; ++k)
                    data->qvel[dof_adr + k] = 0.0;
            }
        }

        // Joint angles back to the spawn pose and velocities zeroed. The commands follow, so the PD
        // controller does not fight the reset.
        for (size_t i = 0; i < articulation.joint_ids.size(); ++i) {
            const int joint = articulation.joint_ids[i];
            if (joint < 0 || model->jnt_type[joint] == mjJNT_FREE)
                continue;
            const auto it = pose.joint_angles.find(articulation.joint_names[i]);
            const float angle = (it != pose.joint_angles.end()) ? it->second : 0.0f;
            data->qpos[model->jnt_qposadr[joint]] = static_cast<double>(angle);
            data->qvel[model->jnt_dofadr[joint]] = 0.0;
            articulation.joint_commands[i].q = angle;
            articulation.joint_commands[i].dq = 0.0f;
        }

        mj_forward(model, data);
        bud::print("[MuJoCo] articulation '{}' reset to its spawn pose", articulation.link_names.empty()
                                                                              ? std::string("(unnamed)")
                                                                              : articulation.link_names[0]);
    }

    int MujocoPhysicsWorld::model_body_count() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return 0;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return model ? model->nbody : 0;
    }

    int MujocoPhysicsWorld::model_actuator_count() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return 0;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return model ? model->nu : 0;
    }

    int MujocoPhysicsWorld::model_sensor_count() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return 0;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return model ? static_cast<int>(model->nsensor) : 0;
    }

    bool MujocoPhysicsWorld::get_articulation_imu(ArticulationHandle handle, ArticulationImu& out) const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!model || !data || !handle.is_valid() || handle.id >= articulations.size())
            return false;
        if (!articulations[handle.id].valid)
            return false;

        const Articulation& articulation = articulations[handle.id];
        const int quat_sensor = articulation.imu_quat_sensor >= 0 ? articulation.imu_quat_sensor : imu_quat_sensor;
        const int gyro_sensor = articulation.imu_gyro_sensor >= 0 ? articulation.imu_gyro_sensor : imu_gyro_sensor;
        const int accel_sensor = articulation.imu_accel_sensor >= 0 ? articulation.imu_accel_sensor : imu_accel_sensor;

        if (quat_sensor < 0)
            return false;

        // framequat is (w, x, y, z) in MuJoCo's Z-up world. Keep the raw robot-frame value for
        // policies and also publish the engine-frame one for engine-side consumers.
        const int quat_adr = model->sensor_adr[quat_sensor];
        const bud::math::quaternion mujoco_orientation(static_cast<float>(data->sensordata[quat_adr + 0]),
                                                       static_cast<float>(data->sensordata[quat_adr + 1]),
                                                       static_cast<float>(data->sensordata[quat_adr + 2]),
                                                       static_cast<float>(data->sensordata[quat_adr + 3]));
        out.orientation_robot = mujoco_orientation;
        out.orientation = from_mujoco(mujoco_orientation);

        // gyro and accelerometer are expressed in the site (root body) frame already, i.e. exactly the
        // robot body frame policies expect, so they are passed through unconverted.
        out.angular_velocity = bud::math::vec3(0.0f);
        if (gyro_sensor >= 0) {
            const int gyro_adr = model->sensor_adr[gyro_sensor];
            out.angular_velocity = bud::math::vec3(static_cast<float>(data->sensordata[gyro_adr + 0]),
                                                   static_cast<float>(data->sensordata[gyro_adr + 1]),
                                                   static_cast<float>(data->sensordata[gyro_adr + 2]));
        }
        out.linear_acceleration = bud::math::vec3(0.0f);
        if (accel_sensor >= 0) {
            const int accel_adr = model->sensor_adr[accel_sensor];
            out.linear_acceleration = bud::math::vec3(static_cast<float>(data->sensordata[accel_adr + 0]),
                                                      static_cast<float>(data->sensordata[accel_adr + 1]),
                                                      static_cast<float>(data->sensordata[accel_adr + 2]));
        }

        // Base linear velocity in the robot body frame, from the root free joint. MuJoCo reports it
        // in its own world frame, so rotate it into the body frame with the raw robot orientation.
        out.linear_velocity = bud::math::vec3(0.0f);
        if (!articulation.body_ids.empty() && articulation.body_ids[0] >= 0) {
            const int root_body = articulation.body_ids[0];
            const int joint = model->body_jntadr[root_body];
            if (joint >= 0 && model->jnt_type[joint] == mjJNT_FREE) {
                const int dof_adr = model->jnt_dofadr[joint];
                const bud::math::vec3 world_velocity(static_cast<float>(data->qvel[dof_adr + 0]),
                                                     static_cast<float>(data->qvel[dof_adr + 1]),
                                                     static_cast<float>(data->qvel[dof_adr + 2]));
                out.linear_velocity = glm::conjugate(mujoco_orientation) * world_velocity;
            }
        }
        return true;
    }

    bool MujocoPhysicsWorld::prepare_simulation() {
        is_compiling_async.store(true, std::memory_order_release);
        struct AsyncGuard {
            std::atomic<bool>& flag;
            ~AsyncGuard() {
                flag.store(false, std::memory_order_release);
            }
        } guard{ is_compiling_async };
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return compile_world();
    }

    bool MujocoPhysicsWorld::compile_world() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!spec_dirty && model)
            return true;

        if (model) {
            // Structural changes (new articulations, new rigid bodies) require a full recompile.
            // Spawn poses are stored separately and reapplied below, so the recompile is safe at
            // any point during init. Dynamic simulation state (velocities, mid-step qpos) is lost
            // but that is expected when the world topology changes.
            bud::print("[MuJoCo] world spec changed, recompiling ({} articulations registered)",
                       articulations.size());
            mj_deleteData(data);
            mj_deleteModel(model);
            data = nullptr;
            model = nullptr;
        }

        // Actuators come from the cooked model, not from here: the offline cook bakes one torque
        // motor per actuated joint (with the URDF effort limit as its ctrlrange) together with the
        // joint's armature/damping/friction loss. That is the official MuJoCo robot model, and the
        // runtime must not add a second set of actuators on the same joints.

        bud::print("[MuJoCo] compiling world spec");
        model = mj_compile(spec, static_cast<mjVFS*>(mesh_vfs));
        if (!model) {
            const char* compile_error = mjs_getError(spec);
            bud::eprint("{}", std::string("[MuJoCo] compile failed: ") +
                        (compile_error ? compile_error : "(no message)"));
            return false;
        }
        data = mj_makeData(model);
        if (!data) {
            bud::eprint("[MuJoCo] mj_makeData failed");
            return false;
        }

        const bud::math::vec3 mujoco_gravity = to_mujoco(world_config.gravity);
        model->opt.gravity[0] = mujoco_gravity.x;
        model->opt.gravity[1] = mujoco_gravity.y;
        model->opt.gravity[2] = mujoco_gravity.z;

        // Stage 4: Newton solver + implicit fast integrator for stiff PD stability
        model->opt.solver = mjSOL_NEWTON;
        model->opt.integrator = mjINT_IMPLICITFAST;
        model->opt.iterations = k_solver_iterations;
        model->opt.timestep = k_solver_timestep;

        // Resolve the names the interface works with into MuJoCo ids.
        body_ids_by_name.clear();
        body_names_by_id.clear();
        for (int body = 0; body < model->nbody; ++body) {
            const char* name = mj_id2name(model, mjOBJ_BODY, body);
            if (!name)
                continue;
            body_ids_by_name[name] = body;
            body_names_by_id[body] = name;
        }
        joint_ids_by_name.clear();
        for (int joint = 0; joint < model->njnt; ++joint) {
            const char* name = mj_id2name(model, mjOBJ_JOINT, joint);
            if (name)
                joint_ids_by_name[name] = joint;
        }
        // Key actuators by the joint they drive instead of by name: the cooked model names its
        // motors "<joint>_motor", and this stays correct whatever the naming convention becomes.
        actuator_ids_by_name.clear();
        for (int actuator = 0; actuator < model->nu; ++actuator) {
            if (model->actuator_trntype[actuator] != mjTRN_JOINT)
                continue;
            const int joint = model->actuator_trnid[2 * actuator];
            if (joint < 0)
                continue;
            const char* joint_name = mj_id2name(model, mjOBJ_JOINT, joint);
            if (!joint_name)
                continue;
            actuator_ids_by_name[joint_name] = actuator;
        }

        // The cooked robot carries an IMU on its root link; resolve the sensors once per compile.
        imu_quat_sensor = mj_name2id(model, mjOBJ_SENSOR, "imu_quat");
        imu_gyro_sensor = mj_name2id(model, mjOBJ_SENSOR, "imu_gyro");
        imu_accel_sensor = mj_name2id(model, mjOBJ_SENSOR, "imu_accel");

        for (auto& articulation : articulations) {
            if (!articulation.valid)
                continue;

            int quat_id = mj_name2id(model, mjOBJ_SENSOR, (articulation.name + "_imu_quat").c_str());
            if (quat_id < 0)
                quat_id = mj_name2id(model, mjOBJ_SENSOR, (articulation.name + "_orientation").c_str());
            if (quat_id < 0 && (articulation.name == "microduck" || articulation.name.find("duck") != std::string::npos))
                quat_id = mj_name2id(model, mjOBJ_SENSOR, "orientation");
            if (quat_id < 0)
                quat_id = imu_quat_sensor;
            articulation.imu_quat_sensor = quat_id;

            int gyro_id = mj_name2id(model, mjOBJ_SENSOR, (articulation.name + "_imu_gyro").c_str());
            if (gyro_id < 0)
                gyro_id = mj_name2id(model, mjOBJ_SENSOR, (articulation.name + "_imu_ang_vel").c_str());
            if (gyro_id < 0 && (articulation.name == "microduck" || articulation.name.find("duck") != std::string::npos)) {
                gyro_id = mj_name2id(model, mjOBJ_SENSOR, "imu_ang_vel");
                if (gyro_id < 0)
                    gyro_id = mj_name2id(model, mjOBJ_SENSOR, "angular-velocity");
            }
            if (gyro_id < 0)
                gyro_id = imu_gyro_sensor;
            articulation.imu_gyro_sensor = gyro_id;

            int accel_id = mj_name2id(model, mjOBJ_SENSOR, (articulation.name + "_imu_accel").c_str());
            if (accel_id < 0)
                accel_id = imu_accel_sensor;
            articulation.imu_accel_sensor = accel_id;
        }

        // Our handles point at bodies by id; resolved by registered name
        for (size_t handle = 0; handle < handle_body_ids.size(); ++handle) {
            if (handle < handle_body_names.size() && !handle_body_names[handle].empty()) {
                const auto it = body_ids_by_name.find(handle_body_names[handle]);
                handle_body_ids[handle] = (it != body_ids_by_name.end()) ? it->second : -1;
            } else {
                handle_body_ids[handle] = -1;
            }
        }

        // Articulation id maps plus the spawn pose. Removed articulations keep their slot (so handles
        // stay stable) but hold no ids and get no spawn pose applied.
        for (size_t index = 0; index < articulations.size(); ++index) {
            Articulation& articulation = articulations[index];
            if (!articulation.valid) {
                articulation.body_ids.assign(articulation.body_ids.size(), -1);
                articulation.joint_ids.assign(articulation.joint_ids.size(), -1);
                articulation.actuator_ids.assign(articulation.actuator_ids.size(), -1);
                continue;
            }
            for (size_t i = 0; i < articulation.link_names.size(); ++i) {
                const auto it = body_ids_by_name.find(articulation.link_names[i]);
                articulation.body_ids[i] = (it != body_ids_by_name.end()) ? it->second : -1;
            }
            for (size_t i = 0; i < articulation.joint_names.size(); ++i) {
                const auto it = joint_ids_by_name.find(articulation.joint_names[i]);
                articulation.joint_ids[i] = (it != joint_ids_by_name.end()) ? it->second : -1;
                const auto act_it = actuator_ids_by_name.find(articulation.joint_names[i]);
                articulation.actuator_ids[i] = (act_it != actuator_ids_by_name.end()) ? act_it->second : -1;
            }

            if (index < spawn_poses.size()) {
                const SpawnPose& pose = spawn_poses[index];
                if (!articulation.link_names.empty() && articulation.body_ids[0] >= 0) {
                    const int root_body = articulation.body_ids[0];
                    const int joint = model->body_jntadr[root_body];
                    if (joint >= 0 && model->jnt_type[joint] == mjJNT_FREE) {
                        const int adr = model->jnt_qposadr[joint];
                        const bud::math::vec3 spawn_position = to_mujoco(pose.position);
                        const bud::math::quaternion spawn_rotation = to_mujoco(pose.rotation);
                        data->qpos[adr + 0] = spawn_position.x;
                        data->qpos[adr + 1] = spawn_position.y;
                        data->qpos[adr + 2] = spawn_position.z;
                        data->qpos[adr + 3] = spawn_rotation.w;
                        data->qpos[adr + 4] = spawn_rotation.x;
                        data->qpos[adr + 5] = spawn_rotation.y;
                        data->qpos[adr + 6] = spawn_rotation.z;
                    }
                }
                for (const auto& [joint_name, angle] : pose.joint_angles) {
                    const auto it = joint_ids_by_name.find(joint_name);
                    if (it == joint_ids_by_name.end())
                        continue;
                    const int joint = it->second;
                    if (model->jnt_type[joint] != mjJNT_HINGE)
                        continue;
                    data->qpos[model->jnt_qposadr[joint]] = angle;
                    const auto cmd_it = articulation.joint_index_by_name.find(joint_name);
                    if (cmd_it != articulation.joint_index_by_name.end())
                        articulation.joint_commands[cmd_it->second].q = angle;
                }
            }
        }

        mj_forward(model, data);
        spec_dirty = false;

        // Collision census and contact parameter tuning for Stage 4
        {
            int colliding = 0;
            int mesh_geoms = 0;
            int visual_only = 0;
            for (int geom = 0; geom < model->ngeom; ++geom) {
                const int contype = model->geom_contype[geom];
                const int conaffinity = model->geom_conaffinity[geom];
                if (contype != 0 || conaffinity != 0)
                    ++colliding;
                else
                    ++visual_only;
                if (model->geom_type[geom] == mjGEOM_MESH)
                    ++mesh_geoms;

                const int body = model->geom_bodyid[geom];
                const char* bname = mj_id2name(model, mjOBJ_BODY, body);
                const std::string_view bname_view = bname ? bname : "";
                const bool is_foot = (bname_view.find("ankle_roll") != std::string_view::npos ||
                                      bname_view.find("ankle_pitch") != std::string_view::npos ||
                                      bname_view.find("foot") != std::string_view::npos);
                const bool is_ground = (body == 0 || bname_view.find("ground") != std::string_view::npos ||
                                        bname_view.find("floor") != std::string_view::npos);

                if (is_foot || is_ground) {
                    model->geom_condim[geom] = 4;
                    model->geom_friction[3 * geom + 0] = 1.0;
                    model->geom_friction[3 * geom + 1] = 0.01;
                    model->geom_friction[3 * geom + 2] = 0.001;
                    model->geom_solref[2 * geom + 0] = 0.004;
                    model->geom_solref[2 * geom + 1] = 1.0;
                    model->geom_solimp[5 * geom + 0] = 0.9;
                    model->geom_solimp[5 * geom + 1] = 0.95;
                    model->geom_solimp[5 * geom + 2] = 0.001;
                    model->geom_solimp[5 * geom + 3] = 0.5;
                    model->geom_solimp[5 * geom + 4] = 2.0;
                }
            }
            bud::print("[MuJoCo] geoms: {} total, {} colliding, {} visual only, {} mesh, {} proxy boxes",
                       model->ngeom, colliding, visual_only, mesh_geoms, proxy_box_static_colliders);
        }
        if (uncollidable_static_meshes > 0) {
            bud::print("[MuJoCo] {} static scene meshes skipped (visual-only; standing surface is the "
                       "configured ground plane)",
                       uncollidable_static_meshes);
        }

        bud::print("{}", "[MuJoCo] world compiled: bodies=" + std::to_string(model->nbody) +
                   " joints=" + std::to_string(model->njnt) +
                   " geoms=" + std::to_string(model->ngeom) +
                   " actuators=" + std::to_string(model->nu));
        return true;
    }

    void MujocoPhysicsWorld::step(float delta_time, int, int) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        // Compiling here would run the expensive MuJoCo build on whichever thread steps first - the
        // main thread during startup - which is the freeze we are removing. prepare_simulation()
        // owns compilation; until it finishes there is nothing to step.
        if (!model || spec_dirty)
            return;

        // MuJoCo wants a small fixed timestep; consume the frame time in substeps and cap the catch
        // up so a long frame cannot run away.
        constexpr int kMaxSubsteps = 40;
        const double timestep = model->opt.timestep > 0.0 ? model->opt.timestep : k_solver_timestep;
        accumulated_time += static_cast<double>(delta_time);
        int substeps = 0;
        while (accumulated_time >= timestep && substeps < kMaxSubsteps) {
            // Apply motor PD torque for all articulations
            for (auto& articulation : articulations) {
                if (!articulation.valid)
                    continue;
                for (size_t i = 0; i < articulation.joint_ids.size(); ++i) {
                    const int joint = articulation.joint_ids[i];
                    const int actuator = articulation.actuator_ids[i];
                    if (joint < 0 || actuator < 0)
                        continue;

                    const auto& cmd = articulation.joint_commands[i];

                    // Cooked assets are not uniform: the G1 carries torque motors, while the
                    // Microduck MJCF declares `<position kp=...>` servos. For a position actuator
                    // `ctrl` is the *target angle*, so writing a PD torque there commands a
                    // nonsense angle (the duck was being driven to ~0 rad and never stepped).
                    // Drive each actuator in its native mode instead.
                    const bool position_servo =
                        model->actuator_biastype[actuator] == mjBIAS_AFFINE &&
                        model->actuator_biasprm[3 * actuator + 1] != 0.0;

                    double control = 0.0;
                    if (position_servo) {
                        // Authored servos often declare kv = 0, leaving a soft, undamped joint that
                        // limit-cycles around its target (the duck jittered with joint velocities of
                        // +-1.8 rad/s). Apply the command's derivative gain as the servo's velocity
                        // term so the commanded stiffness is actually damped.
                        if (model->actuator_biasprm != nullptr) {
                            model->actuator_biasprm[3 * actuator + 1] = -static_cast<double>(cmd.kp);
                            model->actuator_biasprm[3 * actuator + 2] = -static_cast<double>(cmd.kd);
                        }
                        control = static_cast<double>(cmd.q);
                    } else {
                        const double q = data->qpos[model->jnt_qposadr[joint]];
                        const double dq = (model->jnt_type[joint] != mjJNT_FREE)
                                              ? data->qvel[model->jnt_dofadr[joint]]
                                              : 0.0;
                        control = static_cast<double>(cmd.kp) * (static_cast<double>(cmd.q) - q) +
                                  static_cast<double>(cmd.kd) * (static_cast<double>(cmd.dq) - dq) +
                                  static_cast<double>(cmd.tau_ff);
                    }

                    if (model->actuator_ctrllimited[actuator]) {
                        const double lower = model->actuator_ctrlrange[2 * actuator + 0];
                        const double upper = model->actuator_ctrlrange[2 * actuator + 1];
                        control = std::clamp(control, lower, upper);
                    }
                    data->ctrl[actuator] = control;
                }
            }

            mj_step(model, data);
            accumulated_time -= timestep;
            ++substeps;
        }
        if (substeps == kMaxSubsteps)
            accumulated_time = 0.0;

        refresh_body_state();

        // Contact callbacks: MuJoCo reports the current set, so begin/persist/end are derived by
        // diffing against the previous step.
        std::vector<std::pair<int, int>> current_pairs;
        current_pairs.reserve(static_cast<size_t>(data->ncon));
        for (int i = 0; i < data->ncon; ++i) {
            const mjContact& contact = data->contact[i];
            const int body_a = model->geom_bodyid[contact.geom1];
            const int body_b = model->geom_bodyid[contact.geom2];
            current_pairs.emplace_back(body_a, body_b);
            const auto was_present = std::find(previous_contact_pairs.begin(), previous_contact_pairs.end(),
                                               std::make_pair(body_a, body_b));
            const bool is_new = was_present == previous_contact_pairs.end();
            ContactCallback& callback = is_new ? contact_begin_cb : contact_persist_cb;
            if (callback) {
                const bud::math::vec3 point = from_mujoco(bud::math::vec3(
                    static_cast<float>(contact.pos[0]), static_cast<float>(contact.pos[1]),
                    static_cast<float>(contact.pos[2])));
                const bud::math::vec3 normal = direction_from_mujoco(bud::math::vec3(
                    static_cast<float>(contact.frame[0]), static_cast<float>(contact.frame[1]),
                    static_cast<float>(contact.frame[2])));
                callback(body_user_data(body_a), body_user_data(body_b), point, normal,
                         static_cast<float>(-contact.dist));
            }
        }
        if (contact_end_cb) {
            for (const auto& pair : previous_contact_pairs) {
                if (std::find(current_pairs.begin(), current_pairs.end(), pair) == current_pairs.end())
                    contact_end_cb(body_user_data(pair.first), body_user_data(pair.second),
                                   {}, {}, 0.0f);
            }
        }
        previous_contact_pairs = std::move(current_pairs);
    }

    void MujocoPhysicsWorld::refresh_body_state() {
        if (!model || !data)
            return;

        body_state.body_count = 0;
        for (size_t handle = 0; handle < handle_body_ids.size(); ++handle) {
            const int body = handle_body_ids[handle];
            if (body < 0)
                continue;
            const int index = static_cast<int>(body_state.body_count++);
            body_state.body_positions[index] = from_mujoco(bud::math::vec3(
                static_cast<float>(data->xpos[3 * body]), static_cast<float>(data->xpos[3 * body + 1]),
                static_cast<float>(data->xpos[3 * body + 2])));
            body_state.body_rotations[index] = from_mujoco(bud::math::quaternion(
                static_cast<float>(data->xquat[4 * body]), static_cast<float>(data->xquat[4 * body + 1]),
                static_cast<float>(data->xquat[4 * body + 2]), static_cast<float>(data->xquat[4 * body + 3])));
            // MuJoCo's cvel is [angular, linear] about the body's centre of mass.
            body_state.body_angular_velocities[index] = from_mujoco(bud::math::vec3(
                static_cast<float>(data->cvel[6 * body]), static_cast<float>(data->cvel[6 * body + 1]),
                static_cast<float>(data->cvel[6 * body + 2])));
            body_state.body_linear_velocities[index] = from_mujoco(bud::math::vec3(
                static_cast<float>(data->cvel[6 * body + 3]), static_cast<float>(data->cvel[6 * body + 4]),
                static_cast<float>(data->cvel[6 * body + 5])));
            body_state.body_masses[index] = static_cast<float>(model->body_mass[body]);
            body_state.body_flags[index] = handle_static[handle] ? BODY_FLAG_STATIC : 0;
            body_state.body_user_data[index] = handle_user_data[handle];
            body_state.body_half_extents[index] = (handle < handle_half_extents.size())
                ? handle_half_extents[handle]
                : bud::math::vec3(0.0f);
        }
    }

    size_t MujocoPhysicsWorld::get_body_count() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return 0;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return body_state.body_count;
    }

    uint32_t MujocoPhysicsWorld::get_active_body_count() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return static_cast<uint32_t>(body_state.body_count);
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return static_cast<uint32_t>(body_state.body_count);
    }

    const RigidBodyStateSoA& MujocoPhysicsWorld::get_body_states() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return body_state;
        // The reference cannot be kept safe by a lock that is released on return: the SoA is only
        // written inside the locked step(), so callers must not read it while a step is in flight.
        // The scene layer's own mutex is what serialises that with the render thread.
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return body_state;
    }

    int MujocoPhysicsWorld::find_body_id(const std::string& name) const {
        const auto it = body_ids_by_name.find(name);
        return (it != body_ids_by_name.end()) ? it->second : -1;
    }

    int MujocoPhysicsWorld::find_joint_id(const std::string& name) const {
        const auto it = joint_ids_by_name.find(name);
        return (it != joint_ids_by_name.end()) ? it->second : -1;
    }

    int MujocoPhysicsWorld::find_actuator_id(const std::string& name) const {
        const auto it = actuator_ids_by_name.find(name);
        return (it != actuator_ids_by_name.end()) ? it->second : -1;
    }

    void* MujocoPhysicsWorld::body_user_data(int body_id) const {
        for (size_t handle = 0; handle < handle_body_ids.size(); ++handle) {
            if (handle_body_ids[handle] == body_id)
                return handle_user_data[handle];
        }
        return nullptr;
    }

    void MujocoPhysicsWorld::set_body_position(RigidBodyHandle handle, const bud::math::vec3& position) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_qposadr[joint];
        const bud::math::vec3 mujoco_position = to_mujoco(position);
        data->qpos[adr + 0] = mujoco_position.x;
        data->qpos[adr + 1] = mujoco_position.y;
        data->qpos[adr + 2] = mujoco_position.z;
        mj_forward(model, data);
    }

    void MujocoPhysicsWorld::set_body_rotation(RigidBodyHandle handle, const bud::math::quaternion& rotation) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_qposadr[joint];
        const bud::math::quaternion mujoco_rotation = to_mujoco(rotation);
        data->qpos[adr + 3] = mujoco_rotation.w;
        data->qpos[adr + 4] = mujoco_rotation.x;
        data->qpos[adr + 5] = mujoco_rotation.y;
        data->qpos[adr + 6] = mujoco_rotation.z;
        mj_forward(model, data);
    }

    void MujocoPhysicsWorld::set_body_linear_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_dofadr[joint];
        const bud::math::vec3 mujoco_velocity = direction_to_mujoco(velocity);
        data->qvel[adr + 0] = mujoco_velocity.x;
        data->qvel[adr + 1] = mujoco_velocity.y;
        data->qvel[adr + 2] = mujoco_velocity.z;
    }

    void MujocoPhysicsWorld::set_body_angular_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_dofadr[joint];
        const bud::math::vec3 mujoco_velocity = direction_to_mujoco(velocity);
        data->qvel[adr + 3] = mujoco_velocity.x;
        data->qvel[adr + 4] = mujoco_velocity.y;
        data->qvel[adr + 5] = mujoco_velocity.z;
    }

    void MujocoPhysicsWorld::apply_force(RigidBodyHandle handle, const bud::math::vec3& force) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const bud::math::vec3 mujoco_force = direction_to_mujoco(force);
        data->xfrc_applied[6 * body + 0] += mujoco_force.x;
        data->xfrc_applied[6 * body + 1] += mujoco_force.y;
        data->xfrc_applied[6 * body + 2] += mujoco_force.z;
    }

    void MujocoPhysicsWorld::apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0 || model->body_mass[body] <= 0.0f)
            return;
        const double inverse_mass = 1.0 / model->body_mass[body];
        const bud::math::vec3 mujoco_impulse = direction_to_mujoco(impulse);
        const int dof = model->jnt_dofadr[model->body_jntadr[body]];
        data->qvel[dof + 0] += mujoco_impulse.x * inverse_mass;
        data->qvel[dof + 1] += mujoco_impulse.y * inverse_mass;
        data->qvel[dof + 2] += mujoco_impulse.z * inverse_mass;
    }

    bool MujocoPhysicsWorld::get_articulation_state(ArticulationHandle handle, ArticulationStateSoA& out) const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!model || !data || !handle.is_valid() || handle.id >= articulations.size())
            return false;
        const Articulation& articulation = articulations[handle.id];
        if (!articulation.valid)
            return false;

        out.resize(articulation.body_ids.size(), articulation.joint_ids.size());
        for (size_t i = 0; i < articulation.body_ids.size(); ++i) {
            const int body = articulation.body_ids[i];
            if (body < 0)
                continue;
            out.link_positions[i] = from_mujoco(bud::math::vec3(static_cast<float>(data->xpos[3 * body]),
                                                               static_cast<float>(data->xpos[3 * body + 1]),
                                                               static_cast<float>(data->xpos[3 * body + 2])));
            out.link_rotations[i] = from_mujoco(bud::math::quaternion(static_cast<float>(data->xquat[4 * body]),
                                                                     static_cast<float>(data->xquat[4 * body + 1]),
                                                                     static_cast<float>(data->xquat[4 * body + 2]),
                                                                     static_cast<float>(data->xquat[4 * body + 3])));
            out.link_linear_velocities[i] = from_mujoco(bud::math::vec3(static_cast<float>(data->cvel[6 * body + 3]),
                                                                       static_cast<float>(data->cvel[6 * body + 4]),
                                                                       static_cast<float>(data->cvel[6 * body + 5])));
            out.link_angular_velocities[i] = from_mujoco(bud::math::vec3(static_cast<float>(data->cvel[6 * body + 0]),
                                                                        static_cast<float>(data->cvel[6 * body + 1]),
                                                                        static_cast<float>(data->cvel[6 * body + 2])));
        }
        for (size_t i = 0; i < articulation.joint_ids.size(); ++i) {
            const int joint = articulation.joint_ids[i];
            if (joint < 0)
                continue;
            out.joint_positions[i] = static_cast<float>(data->qpos[model->jnt_qposadr[joint]]);
            if (model->jnt_type[joint] != mjJNT_FREE)
                out.joint_velocities[i] = static_cast<float>(data->qvel[model->jnt_dofadr[joint]]);
            const int actuator = articulation.actuator_ids[i];
            if (actuator >= 0)
                out.joint_torques[i] = static_cast<float>(data->actuator_force[actuator]);
        }
        return true;
    }

    void MujocoPhysicsWorld::set_articulation_joint_commands(ArticulationHandle handle,
                                                             std::span<const JointCommand> commands) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        // Commands are stored per joint index, so this needs no compiled model. Compiling here used
        // to trigger the whole MuJoCo build during robot spawn - on the main thread.
        if (!handle.is_valid() || handle.id >= articulations.size())
            return;
        Articulation& articulation = articulations[handle.id];
        if (!articulation.valid)
            return;

        // Name -> index was resolved once at spawn, so each command is one hash lookup.
        for (const auto& cmd : commands) {
            const auto it = articulation.joint_index_by_name.find(cmd.joint_name);
            if (it != articulation.joint_index_by_name.end())
                articulation.joint_commands[it->second] = cmd;
        }
    }

    void MujocoPhysicsWorld::set_articulation_target_angle(ArticulationHandle handle,
                                                          const std::string& joint_name, float angle) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!handle.is_valid() || handle.id >= articulations.size())
            return;
        Articulation& articulation = articulations[handle.id];
        if (!articulation.valid)
            return;

        const auto it = articulation.joint_index_by_name.find(joint_name);
        if (it == articulation.joint_index_by_name.end())
            return;
        const size_t i = it->second;
        articulation.joint_commands[i].q = angle;
        articulation.joint_commands[i].dq = 0.0f;
        if (articulation.joint_commands[i].kp == 0.0f)
            articulation.joint_commands[i].kp = articulation.joint_descs[i].stiffness > 0.0f
                                                    ? articulation.joint_descs[i].stiffness
                                                    : 40.0f;
        if (articulation.joint_commands[i].kd == 0.0f)
            articulation.joint_commands[i].kd = articulation.joint_descs[i].damping > 0.0f
                                                    ? articulation.joint_descs[i].damping
                                                    : 2.0f;
    }

    void MujocoPhysicsWorld::set_articulation_target_velocity(ArticulationHandle handle,
                                                             const std::string& joint_name, float velocity) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!handle.is_valid() || handle.id >= articulations.size())
            return;
        Articulation& articulation = articulations[handle.id];
        if (!articulation.valid)
            return;

        const auto it = articulation.joint_index_by_name.find(joint_name);
        if (it != articulation.joint_index_by_name.end())
            articulation.joint_commands[it->second].dq = velocity;
    }

    float MujocoPhysicsWorld::get_articulation_joint_angle(ArticulationHandle, const std::string& joint_name) const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return 0.0f;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!model || !data)
            return 0.0f;
        const int joint = find_joint_id(joint_name);
        if (joint < 0)
            return 0.0f;
        return static_cast<float>(data->qpos[model->jnt_qposadr[joint]]);
    }

    float MujocoPhysicsWorld::get_articulation_joint_stiffness(ArticulationHandle handle, const std::string& joint_name) const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return 0.0f;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!handle.is_valid() || handle.id >= articulations.size())
            return 0.0f;
        const auto& articulation = articulations[handle.id];
        const auto it = articulation.joint_index_by_name.find(joint_name);
        if (it == articulation.joint_index_by_name.end())
            return 0.0f;
        return articulation.joint_commands[it->second].kp;
    }

    void MujocoPhysicsWorld::set_articulation_link_transform(ArticulationHandle handle,
                                                            const std::string& link_name,
                                                            const bud::math::vec3& position,
                                                            const bud::math::quaternion& rotation) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!compile_world() || !handle.is_valid() || handle.id >= articulations.size())
            return;
        const int body = find_body_id(link_name);
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE) {
            bud::eprint("{}", std::string("[MuJoCo] link '") + link_name + "' is not free-floating; cannot teleport it");
            return;
        }
        const int adr = model->jnt_qposadr[joint];
        const bud::math::vec3 mujoco_position = to_mujoco(position);
        data->qpos[adr + 0] = mujoco_position.x;
        data->qpos[adr + 1] = mujoco_position.y;
        data->qpos[adr + 2] = mujoco_position.z;
        const bud::math::quaternion mujoco_rotation = to_mujoco(rotation);
        data->qpos[adr + 3] = mujoco_rotation.w;
        data->qpos[adr + 4] = mujoco_rotation.x;
        data->qpos[adr + 5] = mujoco_rotation.y;
        data->qpos[adr + 6] = mujoco_rotation.z;

        // Zero residual velocities for all joints in this articulation
        const Articulation& articulation = articulations[handle.id];
        for (int j_id : articulation.joint_ids) {
            if (j_id >= 0 && j_id < model->njnt) {
                const int dof = model->jnt_dofadr[j_id];
                const int num_dof = (model->jnt_type[j_id] == mjJNT_FREE) ? 6 : 1;
                for (int d = 0; d < num_dof; ++d) {
                    data->qvel[dof + d] = 0.0;
                }
            }
        }
        mj_forward(model, data);
    }

    std::optional<RaycastResult> MujocoPhysicsWorld::raycast(const bud::math::vec3& from,
                                                            const bud::math::vec3& to) const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return std::nullopt;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!model || !data)
            return std::nullopt;

        const bud::math::vec3 mujoco_from = to_mujoco(from);
        const bud::math::vec3 mujoco_to = to_mujoco(to);
        const double delta[3] = { mujoco_to.x - mujoco_from.x, mujoco_to.y - mujoco_from.y,
                                  mujoco_to.z - mujoco_from.z };
        const double length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
        if (length < 1.0e-8)
            return std::nullopt;
        const double direction[3] = { delta[0] / length, delta[1] / length, delta[2] / length };
        const double point[3] = { mujoco_from.x, mujoco_from.y, mujoco_from.z };

        int geom_id = -1;
        double hit_normal[3] = { 0.0, 0.0, 0.0 };
        const double distance =
            mj_ray(model, data, point, direction, nullptr, 1, -1, &geom_id, hit_normal);
        if (distance < 0.0 || geom_id < 0 || distance > length)
            return std::nullopt;

        RaycastResult result;
        result.hit = true;
        result.fraction = static_cast<float>(distance / length);
        result.hit_point = from_mujoco(bud::math::vec3(
            mujoco_from.x + static_cast<float>(direction[0] * distance),
            mujoco_from.y + static_cast<float>(direction[1] * distance),
            mujoco_from.z + static_cast<float>(direction[2] * distance)));
        result.hit_normal = direction_from_mujoco(bud::math::vec3(static_cast<float>(hit_normal[0]),
                                                                  static_cast<float>(hit_normal[1]),
                                                                  static_cast<float>(hit_normal[2])));
        result.user_data = body_user_data(model->geom_bodyid[geom_id]);
        return result;
    }

    std::optional<ShapeCastResult> MujocoPhysicsWorld::sphere_cast(const bud::math::vec3& from,
                                                                  const bud::math::vec3& to,
                                                                  float radius) const {
        // MuJoCo has no swept-sphere query. Approximate it with a ray from the sphere centre;
        // a robot project's character controller needs the contact, not the exact sweep.
        auto hit = raycast(from, to);
        if (!hit)
            return std::nullopt;
        ShapeCastResult result;
        result.hit = true;
        result.fraction = hit->fraction;
        result.hit_point = hit->hit_point;
        result.hit_normal = hit->hit_normal;
        result.user_data = hit->user_data;
        (void)radius;
        return result;
    }

    void MujocoPhysicsWorld::set_contact_begin_callback(ContactCallback callback) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        contact_begin_cb = std::move(callback);
    }

    void MujocoPhysicsWorld::set_contact_persist_callback(ContactCallback callback) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        contact_persist_cb = std::move(callback);
    }

    void MujocoPhysicsWorld::set_contact_end_callback(ContactCallback callback) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        contact_end_cb = std::move(callback);
    }

    std::vector<ContactPoint> MujocoPhysicsWorld::get_contacts() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return {};
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::vector<ContactPoint> contacts;
        if (!model || !data)
            return contacts;
        contacts.reserve(static_cast<size_t>(data->ncon));
        for (int i = 0; i < data->ncon; ++i) {
            const mjContact& contact = data->contact[i];
            ContactPoint point;
            point.body_a_user_data = body_user_data(model->geom_bodyid[contact.geom1]);
            point.body_b_user_data = body_user_data(model->geom_bodyid[contact.geom2]);
            point.point = from_mujoco(bud::math::vec3(static_cast<float>(contact.pos[0]),
                                                       static_cast<float>(contact.pos[1]),
                                                       static_cast<float>(contact.pos[2])));
            point.normal = direction_from_mujoco(bud::math::vec3(static_cast<float>(contact.frame[0]),
                                                                 static_cast<float>(contact.frame[1]),
                                                                 static_cast<float>(contact.frame[2])));
            point.penetration_depth = static_cast<float>(-contact.dist);
            contacts.push_back(point);
        }
        return contacts;
    }

    void MujocoPhysicsWorld::set_gravity(const bud::math::vec3& gravity) {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        world_config.gravity = gravity;
        if (model) {
            model->opt.gravity[0] = gravity.x;
            model->opt.gravity[1] = gravity.y;
            model->opt.gravity[2] = gravity.z;
        }
    }

    bud::math::vec3 MujocoPhysicsWorld::get_gravity() const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return bud::math::vec3(0.0f);
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return world_config.gravity;
    }

    void MujocoPhysicsWorld::collect_debug_lines(std::vector<DebugLine>& out_lines) const {
        if (is_compiling_async.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        // Body frames are enough to see the world; MuJoCo's own visualisation is not used.
        if (!model || !data)
            return;
        constexpr float kAxisLength = 0.15f;
        for (size_t handle = 0; handle < handle_body_ids.size(); ++handle) {
            const int body = handle_body_ids[handle];
            if (body < 0)
                continue;
            const bud::math::vec3 origin = from_mujoco(bud::math::vec3(
                static_cast<float>(data->xpos[3 * body]), static_cast<float>(data->xpos[3 * body + 1]),
                static_cast<float>(data->xpos[3 * body + 2])));
            out_lines.push_back({ origin, origin + from_mujoco(bud::math::vec3(kAxisLength, 0.0f, 0.0f)),
                                  bud::math::vec3(1, 0, 0) });
            out_lines.push_back({ origin, origin + from_mujoco(bud::math::vec3(0.0f, kAxisLength, 0.0f)),
                                  bud::math::vec3(0, 1, 0) });
            out_lines.push_back({ origin, origin + from_mujoco(bud::math::vec3(0.0f, 0.0f, kAxisLength)),
                                  bud::math::vec3(0, 0, 1) });
        }
    }

} // namespace bud::physics
