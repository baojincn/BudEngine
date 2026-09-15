#include "src/physics/bud.physics.world.mujoco.hpp"

#include "src/core/bud.asset.types.hpp"
#include "src/core/bud.logger.hpp"
#include "src/core/bud.raw_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <mujoco/mujoco.h>

namespace bud::physics {

    namespace {

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
            case bud::physics::ShapeType::Box:
            default: return mjGEOM_BOX;
            }
        }

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
        handle_user_data.assign(config.max_bodies, nullptr);
        handle_static.assign(config.max_bodies, false);
        spec_dirty = true;
        bud::print("[MuJoCo] world initialised (bodies are compiled on the first step)");
        return true;
    }

    RigidBodyHandle MujocoPhysicsWorld::add_rigid_body(const RigidBodyDesc& desc) {
        mjsBody* world_body = mjs_findBody(spec, "world");
        if (!world_body) {
            bud::eprint("[MuJoCo] world body missing from the spec");
            return {};
        }

        if (desc.shape.type == ShapeType::Mesh || desc.shape.type == ShapeType::ConvexHull) {
            // Triangle meshes and hulls are game-world geometry; a robot project's world is simple
            // primitives. Falls back to the shape's half extents so the body still exists.
            bud::eprint("[MuJoCo] mesh/convex colliders are not supported by this backend; using a box");
        }

        mjsBody* body = mjs_addBody(world_body, nullptr);
        const std::string body_name = "body_" + std::to_string(handle_body_ids.size());
        mjs_setName(body->element, body_name.c_str());
        body->pos[0] = desc.position.x;
        body->pos[1] = desc.position.y;
        body->pos[2] = desc.position.z;
        body->quat[0] = desc.rotation.w;
        body->quat[1] = desc.rotation.x;
        body->quat[2] = desc.rotation.y;
        body->quat[3] = desc.rotation.z;

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
        default:
            geom->size[0] = desc.shape.half_extent.x;
            geom->size[1] = desc.shape.half_extent.y;
            geom->size[2] = desc.shape.half_extent.z;
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
        handle_user_data.push_back(nullptr);
        handle_static.push_back(desc.motion_type != MotionType::Dynamic);
        spec_dirty = true;
        return RigidBodyHandle{ handle_id };
    }

    void MujocoPhysicsWorld::remove_rigid_body(RigidBodyHandle) {
        // Rebuilding the spec would reset the simulation; removal is not needed by the robot flow
        // yet, so it is reported instead of silently invalidating handles.
        bud::eprint("[MuJoCo] remove_rigid_body is not implemented yet");
    }

    ArticulationHandle MujocoPhysicsWorld::create_articulation(const ArticulationDesc& desc) {
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

        // Serve the meshes the model asks for out of our own assets.
        for (const auto& mesh : desc.cooked_model.meshes) {
            const std::filesystem::path asset_path = std::filesystem::path(world_config.asset_root) / mesh.asset_path;
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
            mj_addBufferVFS(static_cast<mjVFS*>(mesh_vfs), mesh.model_name.c_str(), stl.data(), static_cast<int>(stl.size()));
        }

        bud::print("[MuJoCo] mesh VFS ready ({} buffers)", mesh_buffers.size());
        char error[2048] = { 0 };
        const std::string mjcf(reinterpret_cast<const char*>(desc.cooked_model.payload.data()),
                               desc.cooked_model.payload.size());
        mjSpec* robot_spec = mj_parseXMLString(mjcf.c_str(), static_cast<mjVFS*>(mesh_vfs), error, sizeof(error));
        if (!robot_spec) {
            bud::eprint("{}", std::string("[MuJoCo] parsing the cooked model failed: ") + error);
            return {};
        }

        bud::print("[MuJoCo] robot model parsed");
        mjsBody* world_body = mjs_findBody(spec, "world");
        mjsBody* robot_root = mjs_findBody(robot_spec, desc.root_link.c_str());
        if (!world_body || !robot_root) {
            bud::eprint("{}", "[MuJoCo] could not find the world body or the robot root link '" + desc.root_link + "'");
            mj_deleteSpec(robot_spec);
            return {};
        }

        // Attach the whole robot spec into our world. Passing the spec's own element (elemtype
        // mjOBJ_MODEL) is what mjs_attach is built for: it wires the child's worldbody through a
        // frame and moves its children under ours, free joint included.
        const mjsElement* attached = mjs_attach(world_body->element, robot_spec->element, "", "");
        mj_deleteSpec(robot_spec);
        bud::print("[MuJoCo] robot attach {}", attached ? "ok" : "failed");
        if (!attached) {
            bud::eprint("[MuJoCo] attaching the robot to the world failed");
            return {};
        }

        Articulation articulation;
        articulation.valid = true;
        for (const auto& link : desc.links)
            articulation.link_names.push_back(link.name);
        for (const auto& joint : desc.joints)
            articulation.joint_names.push_back(joint.name);
        articulation.body_ids.assign(articulation.link_names.size(), -1);
        articulation.joint_ids.assign(articulation.joint_names.size(), -1);
        articulation.actuator_ids.assign(articulation.joint_names.size(), -1);
        articulations.push_back(std::move(articulation));

        spawn_poses.push_back({ desc.root_position, desc.root_rotation, desc.initial_joint_angles });
        spec_dirty = true;

        const uint32_t index = static_cast<uint32_t>(articulations.size() - 1);
        bud::print("{}", "[MuJoCo] articulation '" + desc.name + "' attached (" +
                   std::to_string(desc.links.size()) + " links, " +
                   std::to_string(desc.joints.size()) + " joints)");
        return ArticulationHandle{ index };
    }

    void MujocoPhysicsWorld::remove_articulation(ArticulationHandle) {
        bud::eprint("[MuJoCo] remove_articulation is not implemented yet");
    }

    bool MujocoPhysicsWorld::compile_world() {
        if (!spec_dirty && model)
            return true;

        if (model) {
            // State cannot survive a recompile: report it rather than pretending it works.
            bud::eprint("[MuJoCo] recompiling the world resets the simulation state");
            mj_deleteData(data);
            mj_deleteModel(model);
            data = nullptr;
            model = nullptr;
        }

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

        model->opt.gravity[0] = world_config.gravity.x;
        model->opt.gravity[1] = world_config.gravity.y;
        model->opt.gravity[2] = world_config.gravity.z;

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
        actuator_ids_by_name.clear();
        for (int actuator = 0; actuator < model->nu; ++actuator) {
            const char* name = mj_id2name(model, mjOBJ_ACTUATOR, actuator);
            if (name)
                actuator_ids_by_name[name] = actuator;
        }

        // Our handles point at bodies by id; scene bodies were named body_<n>.
        for (size_t handle = 0; handle < handle_body_ids.size(); ++handle) {
            const auto it = body_ids_by_name.find("body_" + std::to_string(handle));
            handle_body_ids[handle] = (it != body_ids_by_name.end()) ? it->second : -1;
        }

        // Articulation id maps plus the spawn pose.
        for (size_t index = 0; index < articulations.size(); ++index) {
            Articulation& articulation = articulations[index];
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
                        data->qpos[adr + 0] = pose.position.x;
                        data->qpos[adr + 1] = pose.position.y;
                        data->qpos[adr + 2] = pose.position.z;
                        data->qpos[adr + 3] = pose.rotation.w;
                        data->qpos[adr + 4] = pose.rotation.x;
                        data->qpos[adr + 5] = pose.rotation.y;
                        data->qpos[adr + 6] = pose.rotation.z;
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
                    // Hold the spawn pose from the first step: bias the position actuator.
                    const auto act_it = actuator_ids_by_name.find(joint_name);
                    if (act_it != actuator_ids_by_name.end())
                        data->ctrl[act_it->second] = angle;
                }
            }
        }

        mj_forward(model, data);
        spec_dirty = false;
        bud::print("{}", "[MuJoCo] world compiled: bodies=" + std::to_string(model->nbody) +
                   " joints=" + std::to_string(model->njnt) +
                   " geoms=" + std::to_string(model->ngeom) +
                   " actuators=" + std::to_string(model->nu));
        return true;
    }

    void MujocoPhysicsWorld::step(float delta_time, int, int) {
        if (!compile_world())
            return;

        // MuJoCo wants a small fixed timestep; consume the frame time in substeps and cap the catch
        // up so a long frame cannot run away.
        constexpr int kMaxSubsteps = 40;
        const double timestep = model->opt.timestep > 0.0 ? model->opt.timestep : 0.002;
        accumulated_time += static_cast<double>(delta_time);
        int substeps = 0;
        while (accumulated_time >= timestep && substeps < kMaxSubsteps) {
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
                const bud::math::vec3 point(static_cast<float>(contact.pos[0]),
                                            static_cast<float>(contact.pos[1]),
                                            static_cast<float>(contact.pos[2]));
                const bud::math::vec3 normal(static_cast<float>(contact.frame[0]),
                                             static_cast<float>(contact.frame[1]),
                                             static_cast<float>(contact.frame[2]));
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
            body_state.body_positions[index] = bud::math::vec3(
                static_cast<float>(data->xpos[3 * body]), static_cast<float>(data->xpos[3 * body + 1]),
                static_cast<float>(data->xpos[3 * body + 2]));
            body_state.body_rotations[index] = bud::math::quaternion(
                static_cast<float>(data->xquat[4 * body]), static_cast<float>(data->xquat[4 * body + 1]),
                static_cast<float>(data->xquat[4 * body + 2]), static_cast<float>(data->xquat[4 * body + 3]));
            // MuJoCo's cvel is [angular, linear] about the body's centre of mass.
            body_state.body_angular_velocities[index] = bud::math::vec3(
                static_cast<float>(data->cvel[6 * body]), static_cast<float>(data->cvel[6 * body + 1]),
                static_cast<float>(data->cvel[6 * body + 2]));
            body_state.body_linear_velocities[index] = bud::math::vec3(
                static_cast<float>(data->cvel[6 * body + 3]), static_cast<float>(data->cvel[6 * body + 4]),
                static_cast<float>(data->cvel[6 * body + 5]));
            body_state.body_masses[index] = static_cast<float>(model->body_mass[body]);
            body_state.body_flags[index] = handle_static[handle] ? BODY_FLAG_STATIC : 0;
            body_state.body_user_data[index] = handle_user_data[handle];
        }
    }

    size_t MujocoPhysicsWorld::get_body_count() const {
        return body_state.body_count;
    }

    uint32_t MujocoPhysicsWorld::get_active_body_count() const {
        return static_cast<uint32_t>(body_state.body_count);
    }

    const RigidBodyStateSoA& MujocoPhysicsWorld::get_body_states() const {
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
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_qposadr[joint];
        data->qpos[adr + 0] = position.x;
        data->qpos[adr + 1] = position.y;
        data->qpos[adr + 2] = position.z;
        mj_forward(model, data);
    }

    void MujocoPhysicsWorld::set_body_rotation(RigidBodyHandle handle, const bud::math::quaternion& rotation) {
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_qposadr[joint];
        data->qpos[adr + 3] = rotation.w;
        data->qpos[adr + 4] = rotation.x;
        data->qpos[adr + 5] = rotation.y;
        data->qpos[adr + 6] = rotation.z;
        mj_forward(model, data);
    }

    void MujocoPhysicsWorld::set_body_linear_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) {
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_dofadr[joint];
        data->qvel[adr + 0] = velocity.x;
        data->qvel[adr + 1] = velocity.y;
        data->qvel[adr + 2] = velocity.z;
    }

    void MujocoPhysicsWorld::set_body_angular_velocity(RigidBodyHandle handle, const bud::math::vec3& velocity) {
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        const int joint = model->body_jntadr[body];
        if (joint < 0 || model->jnt_type[joint] != mjJNT_FREE)
            return;
        const int adr = model->jnt_dofadr[joint];
        data->qvel[adr + 3] = velocity.x;
        data->qvel[adr + 4] = velocity.y;
        data->qvel[adr + 5] = velocity.z;
    }

    void MujocoPhysicsWorld::apply_force(RigidBodyHandle handle, const bud::math::vec3& force) {
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0)
            return;
        data->xfrc_applied[6 * body + 0] += force.x;
        data->xfrc_applied[6 * body + 1] += force.y;
        data->xfrc_applied[6 * body + 2] += force.z;
    }

    void MujocoPhysicsWorld::apply_impulse(RigidBodyHandle handle, const bud::math::vec3& impulse) {
        if (!compile_world() || !handle.is_valid() || handle.id >= handle_body_ids.size())
            return;
        const int body = handle_body_ids[handle.id];
        if (body < 0 || model->body_mass[body] <= 0.0f)
            return;
        const double inverse_mass = 1.0 / model->body_mass[body];
        data->qvel[model->jnt_dofadr[model->body_jntadr[body]] + 0] += impulse.x * inverse_mass;
        data->qvel[model->jnt_dofadr[model->body_jntadr[body]] + 1] += impulse.y * inverse_mass;
        data->qvel[model->jnt_dofadr[model->body_jntadr[body]] + 2] += impulse.z * inverse_mass;
    }

    bool MujocoPhysicsWorld::get_articulation_state(ArticulationHandle handle, ArticulationStateSoA& out) const {
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
            out.link_positions[i] = bud::math::vec3(static_cast<float>(data->xpos[3 * body]),
                                                   static_cast<float>(data->xpos[3 * body + 1]),
                                                   static_cast<float>(data->xpos[3 * body + 2]));
            out.link_rotations[i] = bud::math::quaternion(static_cast<float>(data->xquat[4 * body]),
                                                         static_cast<float>(data->xquat[4 * body + 1]),
                                                         static_cast<float>(data->xquat[4 * body + 2]),
                                                         static_cast<float>(data->xquat[4 * body + 3]));
            out.link_linear_velocities[i] = bud::math::vec3(static_cast<float>(data->cvel[6 * body + 3]),
                                                           static_cast<float>(data->cvel[6 * body + 4]),
                                                           static_cast<float>(data->cvel[6 * body + 5]));
            out.link_angular_velocities[i] = bud::math::vec3(static_cast<float>(data->cvel[6 * body + 0]),
                                                            static_cast<float>(data->cvel[6 * body + 1]),
                                                            static_cast<float>(data->cvel[6 * body + 2]));
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

    void MujocoPhysicsWorld::set_articulation_target_angle(ArticulationHandle handle,
                                                          const std::string& joint_name, float angle) {
        if (!compile_world() || !handle.is_valid() || handle.id >= articulations.size())
            return;
        const int actuator = find_actuator_id(joint_name);
        if (actuator < 0)
            return;
        data->ctrl[actuator] = angle;
    }

    void MujocoPhysicsWorld::set_articulation_target_velocity(ArticulationHandle, const std::string&, float) {
        // The cooked model uses position actuators; velocity targets would need a different actuator
        // configuration, so this is reported instead of silently doing nothing useful.
        bud::eprint("[MuJoCo] set_articulation_target_velocity needs a velocity actuator in the model");
    }

    float MujocoPhysicsWorld::get_articulation_joint_angle(ArticulationHandle, const std::string& joint_name) const {
        if (!model || !data)
            return 0.0f;
        const int joint = find_joint_id(joint_name);
        if (joint < 0)
            return 0.0f;
        return static_cast<float>(data->qpos[model->jnt_qposadr[joint]]);
    }

    float MujocoPhysicsWorld::get_articulation_joint_stiffness(ArticulationHandle, const std::string& joint_name) const {
        if (!model)
            return 0.0f;
        const int actuator = find_actuator_id(joint_name);
        if (actuator < 0)
            return 0.0f;
        return static_cast<float>(model->actuator_gainprm[actuator * mjNGAIN + 0]);
    }

    void MujocoPhysicsWorld::set_articulation_link_transform(ArticulationHandle handle,
                                                            const std::string& link_name,
                                                            const bud::math::vec3& position,
                                                            const bud::math::quaternion& rotation) {
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
        data->qpos[adr + 0] = position.x;
        data->qpos[adr + 1] = position.y;
        data->qpos[adr + 2] = position.z;
        data->qpos[adr + 3] = rotation.w;
        data->qpos[adr + 4] = rotation.x;
        data->qpos[adr + 5] = rotation.y;
        data->qpos[adr + 6] = rotation.z;
        mj_forward(model, data);
    }

    std::optional<RaycastResult> MujocoPhysicsWorld::raycast(const bud::math::vec3& from,
                                                            const bud::math::vec3& to) const {
        if (!model || !data)
            return std::nullopt;

        const double delta[3] = { to.x - from.x, to.y - from.y, to.z - from.z };
        const double length = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1] + delta[2] * delta[2]);
        if (length < 1.0e-8)
            return std::nullopt;
        const double direction[3] = { delta[0] / length, delta[1] / length, delta[2] / length };
        const double point[3] = { from.x, from.y, from.z };

        int geom_id = -1;
        double hit_normal[3] = { 0.0, 0.0, 0.0 };
        const double distance =
            mj_ray(model, data, point, direction, nullptr, 1, -1, &geom_id, hit_normal);
        if (distance < 0.0 || geom_id < 0 || distance > length)
            return std::nullopt;

        RaycastResult result;
        result.hit = true;
        result.fraction = static_cast<float>(distance / length);
        result.hit_point = bud::math::vec3(from.x + static_cast<float>(direction[0] * distance),
                                           from.y + static_cast<float>(direction[1] * distance),
                                           from.z + static_cast<float>(direction[2] * distance));
        result.hit_normal = bud::math::vec3(static_cast<float>(hit_normal[0]),
                                            static_cast<float>(hit_normal[1]),
                                            static_cast<float>(hit_normal[2]));
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
        contact_begin_cb = std::move(callback);
    }

    void MujocoPhysicsWorld::set_contact_persist_callback(ContactCallback callback) {
        contact_persist_cb = std::move(callback);
    }

    void MujocoPhysicsWorld::set_contact_end_callback(ContactCallback callback) {
        contact_end_cb = std::move(callback);
    }

    std::vector<ContactPoint> MujocoPhysicsWorld::get_contacts() const {
        std::vector<ContactPoint> contacts;
        if (!model || !data)
            return contacts;
        contacts.reserve(static_cast<size_t>(data->ncon));
        for (int i = 0; i < data->ncon; ++i) {
            const mjContact& contact = data->contact[i];
            ContactPoint point;
            point.body_a_user_data = body_user_data(model->geom_bodyid[contact.geom1]);
            point.body_b_user_data = body_user_data(model->geom_bodyid[contact.geom2]);
            point.point = bud::math::vec3(static_cast<float>(contact.pos[0]),
                                          static_cast<float>(contact.pos[1]),
                                          static_cast<float>(contact.pos[2]));
            point.normal = bud::math::vec3(static_cast<float>(contact.frame[0]),
                                           static_cast<float>(contact.frame[1]),
                                           static_cast<float>(contact.frame[2]));
            point.penetration_depth = static_cast<float>(-contact.dist);
            contacts.push_back(point);
        }
        return contacts;
    }

    void MujocoPhysicsWorld::set_gravity(const bud::math::vec3& gravity) {
        world_config.gravity = gravity;
        if (model) {
            model->opt.gravity[0] = gravity.x;
            model->opt.gravity[1] = gravity.y;
            model->opt.gravity[2] = gravity.z;
        }
    }

    bud::math::vec3 MujocoPhysicsWorld::get_gravity() const {
        return world_config.gravity;
    }

    void MujocoPhysicsWorld::collect_debug_lines(std::vector<DebugLine>& out_lines) const {
        // Body frames are enough to see the world; MuJoCo's own visualisation is not used.
        if (!model || !data)
            return;
        constexpr float kAxisLength = 0.15f;
        for (size_t handle = 0; handle < handle_body_ids.size(); ++handle) {
            const int body = handle_body_ids[handle];
            if (body < 0)
                continue;
            const float px = static_cast<float>(data->xpos[3 * body]);
            const float py = static_cast<float>(data->xpos[3 * body + 1]);
            const float pz = static_cast<float>(data->xpos[3 * body + 2]);
            const bud::math::vec3 origin(px, py, pz);
            out_lines.push_back({ origin, bud::math::vec3(px + kAxisLength, py, pz), bud::math::vec3(1, 0, 0) });
            out_lines.push_back({ origin, bud::math::vec3(px, py + kAxisLength, pz), bud::math::vec3(0, 1, 0) });
            out_lines.push_back({ origin, bud::math::vec3(px, py, pz + kAxisLength), bud::math::vec3(0, 0, 1) });
        }
    }

} // namespace bud::physics
