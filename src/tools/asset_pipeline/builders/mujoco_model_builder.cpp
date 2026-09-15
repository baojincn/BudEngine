#include "builders/mujoco_model_builder.hpp"

#include "core/support.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <mujoco/mujoco.h>
#include <nlohmann/json.hpp>

namespace bud::asset_pipeline {

    namespace {

        namespace fs = std::filesystem;

        std::string lower_extension(const fs::path& path) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        std::string file_stem_lower(const fs::path& path) {
            std::string stem = path.stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return stem;
        }

        const MujocoJointGroup* find_group(const std::vector<MujocoJointGroup>& groups,
                                           const std::string& joint_name) {
            for (const MujocoJointGroup& group : groups) {
                if (!group.token.empty() && joint_name.find(group.token) != std::string::npos)
                    return &group;
            }
            return nullptr;
        }

        // MuJoCo loads STL/OBJ (not DAE), so only those are registered in the cook VFS.
        bool is_mujoco_mesh(const fs::path& path) {
            const std::string ext = lower_extension(path);
            return ext == ".stl" || ext == ".obj";
        }

    } // namespace

    std::optional<bud::robots::MujocoModelData> cook_mujoco_model(
        const std::string& urdf_path,
        const std::string& package_root,
        const bud::robots::RobotDef& robot_def,
        const MujocoCookOptions& options) {

        if (!fs::exists(urdf_path)) {
            support::log_warn("[MujocoCook] URDF not found: " + urdf_path);
            return std::nullopt;
        }

        // MuJoCo resolves the URDF's mesh paths relative to the URDF's directory, so the cook VFS
        // mirrors that layout. At runtime the same names are served from our asset chunks instead,
        // which is why the payload carries the mesh -> asset mapping.
        mjVFS vfs;
        mj_defaultVFS(&vfs);
        const fs::path urdf_fs_path(urdf_path);
        mj_addFileVFS(&vfs, urdf_fs_path.parent_path().string().c_str(),
                      urdf_fs_path.filename().string().c_str());

        size_t mesh_files = 0;
        if (!package_root.empty() && fs::exists(package_root)) {
            for (const auto& entry : fs::recursive_directory_iterator(package_root)) {
                if (!entry.is_regular_file() || !is_mujoco_mesh(entry.path()))
                    continue;
                const std::string relative =
                    fs::relative(entry.path(), package_root).generic_string();
                if (mj_addFileVFS(&vfs, package_root.c_str(), relative.c_str()) == 0)
                    ++mesh_files;
            }
        }
        support::log_info("[MujocoCook] VFS: meshes=" + std::to_string(mesh_files));

        char error[2048] = { 0 };
        mjSpec* spec = mj_parseXML(urdf_fs_path.filename().string().c_str(), &vfs, error, sizeof(error));
        if (!spec) {
            support::log_warn(std::string("[MujocoCook] MuJoCo parse failed: ") + error);
            mj_deleteVFS(&vfs);
            return std::nullopt;
        }

        // Normalization #1: free base joint. The URDF models the base as world --floating--> root
        // and MuJoCo's importer drops that joint, welding the robot to the world.
        if (mjsBody* root_body = mjs_findBody(spec, robot_def.root_link.c_str())) {
            if (mjs_addFreeJoint(root_body) == nullptr)
                support::log_warn("[MujocoCook] Could not add the free base joint to '" + robot_def.root_link + "'");
        } else {
            support::log_warn("[MujocoCook] Root link body '" + robot_def.root_link + "' not found in the MuJoCo spec");
        }

        // Normalization #2: per-joint motor model. URDF has neither armature (rotor inertia) nor
        // actuator gains, and those are exactly the sim-to-real parameters.
        //
        // Order matters: MuJoCo's spec has to be compiled once *before* actuators are added (adding
        // them to a never-compiled spec crashes inside mj_compile; verified with the standalone
        // spike, where the same compile -> add actuators -> compile sequence works).
        {
            mjModel* probe_model = mj_compile(spec, &vfs);
            if (!probe_model) {
                const char* probe_error = mjs_getError(spec);
                support::log_warn(std::string("[MujocoCook] First (no actuator) compile failed: ") +
                                  (probe_error ? probe_error : "(no message)"));
                mj_deleteSpec(spec);
                mj_deleteVFS(&vfs);
                return std::nullopt;
            }
            mj_deleteModel(probe_model);
        }

        int actuators = 0;
        for (const auto& joint : robot_def.joints) {
            const bool actuated = joint.type == bud::robots::JointType::Revolute ||
                                  joint.type == bud::robots::JointType::Continuous ||
                                  joint.type == bud::robots::JointType::Prismatic;
            if (!actuated)
                continue;

            mjsElement* joint_element = mjs_findElement(spec, mjOBJ_JOINT, joint.name.c_str());
            if (!joint_element) {
                support::log_warn("[MujocoCook] Joint '" + joint.name + "' missing from the MuJoCo spec");
                continue;
            }
            (void)joint_element;

            const MujocoJointGroup* group = find_group(options.joint_groups, joint.name);
            const double kp = group ? group->kp : 40.0;
            const double kv = group ? group->kv : 2.0;

            // Joint damping/frictionloss are imported from the URDF by MuJoCo itself, and armature
            // (plus any override) is injected into the saved MJCF below: writing these spec fields
            // before compiling crashes mj_compile on this model, while the identical spike - which
            // only adds actuators - compiles fine.

            mjsActuator* actuator = mjs_addActuator(spec, nullptr);
            mjs_setName(actuator->element, joint.name.c_str());
            actuator->trntype = mjTRN_JOINT;
            if (actuator->target != nullptr)
                mjs_setString(actuator->target, joint.name.c_str());

            // Exactly one of kv / dampratio may be given; a non-null pointer counts as given, and
            // success is reported as an empty (non-null) message.
            double kv_array[1] = { kv };
            const char* actuator_error = mjs_setToPosition(actuator, kp, kv_array, nullptr, nullptr, 0.0);
            if (actuator_error != nullptr && actuator_error[0] != '\0') {
                support::log_warn(std::string("[MujocoCook] Actuator for '") + joint.name +
                                  "' failed: " + actuator_error);
                continue;
            }
            const double max_torque = (group && group->max_torque > 0.0) ? group->max_torque
                                                                        : joint.limit.effort;
            if (max_torque > 0.0) {
                actuator->forcelimited = 1;
                actuator->forcerange[0] = -max_torque;
                actuator->forcerange[1] = max_torque;
            }
            ++actuators;
        }
        support::log_info("[MujocoCook] actuators=" + std::to_string(actuators));

        support::log_info("[MujocoCook] phase: options");
        // Physics options are applied *after* the compile, right before the model is written out as
        // MJCF (setting them on the spec before compiling crashed inside mj_compile on this URDF).
        // They are part of the saved MJCF, so the runtime compiles them in.
        auto apply_physics_options = [&]() {
            spec->option.timestep = options.timestep;
            spec->option.iterations = options.solver_iterations;
            if (options.contact_noslip)
                spec->option.noslip_iterations = 5;
        };
        (void)apply_physics_options;

        support::log_info("[MujocoCook] phase: compile with actuators");
        mjModel* model = mj_compile(spec, &vfs);
        support::log_info("[MujocoCook] phase: compiled");
        if (!model) {
            const char* compile_error = mjs_getError(spec);
            support::log_warn(std::string("[MujocoCook] MuJoCo compile failed: ") +
                              (compile_error ? compile_error : "(no message)"));
            mj_deleteSpec(spec);
            mj_deleteVFS(&vfs);
            return std::nullopt;
        }

        // Save the normalized model: the runtime compiles this, so the runtime never sees the URDF.
        // MJCF text is preferred (readable, version independent, meshes stay logical references).
        // MuJoCo's XML writers are picky about spec-built models, so fall back to the compiled MJB
        // binary, which works for any model and embeds the collision mesh data.
        apply_physics_options();

        bud::robots::MujocoModelData data;
        {
            const fs::path temp_xml = fs::temp_directory_path() / (robot_def.name + "_cooked.xml");
            if (mj_saveXML(spec, temp_xml.string().c_str(), error, sizeof(error)) == 0) {
                std::ifstream in(temp_xml, std::ios::binary);
                std::stringstream buffer;
                buffer << in.rdbuf();
                data.model_payload = buffer.str();
                data.format = bud::robots::MujocoModelFormat::Mjcf;
            }
            std::error_code remove_error;
            fs::remove(temp_xml, remove_error);
        }

        if (data.model_payload.empty()) {
            const fs::path temp_mjb = fs::temp_directory_path() / (robot_def.name + "_cooked.mjb");
            mj_saveModel(model, temp_mjb.string().c_str(), nullptr, 0);
            std::ifstream in(temp_mjb, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            data.model_payload = buffer.str();
            data.format = bud::robots::MujocoModelFormat::Mjb;
            std::error_code remove_error;
            fs::remove(temp_mjb, remove_error);
        }

        support::log_info(std::string("[MujocoCook] model payload: ") +
                          (data.format == bud::robots::MujocoModelFormat::Mjcf ? "MJCF text" : "MJB binary") +
                          " " + std::to_string(data.model_payload.size()) + " bytes");
        if (data.model_payload.empty()) {
            support::log_warn("[MujocoCook] Neither MJCF nor MJB could be produced; the robot asset "
                              "will carry an empty physics model.");
        }

        // Bound collision hull complexity for the MJCF path. maxhullvert is a per-mesh attribute and
        // MuJoCo's default is generous; a 32-vertex budget keeps contacts cheap and numerically calm
        // (matching the budget the Jolt-cooked hulls used). In the MJB fallback the hulls are already
        // compiled in, so the cap cannot be applied there - see the log below.
        if (data.format == bud::robots::MujocoModelFormat::Mjcf && !data.model_payload.empty()) {
            std::string mjcf_text = std::move(data.model_payload);
            const std::string attribute = " maxhullvert=\"" + std::to_string(options.max_hull_vertices) + "\"";
            std::string patched;
            patched.reserve(mjcf_text.size() + 64 * 32);
            size_t cursor = 0;
            const std::string tag = "<mesh ";
            while (true) {
                const size_t at = mjcf_text.find(tag, cursor);
                if (at == std::string::npos) {
                    patched.append(mjcf_text, cursor, std::string::npos);
                    break;
                }
                const size_t name_at = mjcf_text.find(" name=", at);
                const size_t name_end = (name_at == std::string::npos) ? std::string::npos
                                                                      : mjcf_text.find('"', name_at + 7);
                if (name_end == std::string::npos) {
                    patched.append(mjcf_text, cursor, std::string::npos);
                    break;
                }
                patched.append(mjcf_text, cursor, name_end + 1 - cursor);
                patched.append(attribute);
                cursor = name_end + 1;
            }
            data.model_payload = std::move(patched);
        }

        // Mesh references: built from our own robot definition, not from MuJoCo's name tables (their
        // address arithmetic produced garbage fragments like 'rld'/'h_link'). The MJCF names each
        // mesh with the path the URDF used, so every mesh our definition references maps to the
        // cooked visual asset with the same stem - the runtime then serves MuJoCo those bytes out of
        // the RawMesh chunk already in the package.
        {
            std::unordered_map<std::string, std::string> stem_to_asset;
            for (const auto& link : robot_def.links) {
                for (const auto& visual : link.visuals) {
                    if (visual.geometry.mesh_path.empty())
                        continue;
                    stem_to_asset.emplace(file_stem_lower(fs::path(visual.geometry.mesh_path)),
                                          visual.geometry.mesh_path);
                }
            }

            std::unordered_set<std::string> seen;
            for (const auto& link : robot_def.links) {
                const auto add_mesh = [&](const bud::robots::GeometryDef& geometry) {
                    if (geometry.type != bud::robots::GeometryType::Mesh || geometry.mesh_path.empty())
                        return;
                    if (!seen.insert(geometry.mesh_path).second)
                        return;
                    bud::robots::MujocoMeshRef ref{};
                    ref.mjcf_name = geometry.mesh_path;
                    const auto it = stem_to_asset.find(file_stem_lower(fs::path(geometry.mesh_path)));
                    ref.asset_path = (it != stem_to_asset.end()) ? it->second : geometry.mesh_path;
                    if (it == stem_to_asset.end())
                        support::log_warn("[MujocoCook] No cooked visual asset for mesh '" + geometry.mesh_path + "'");
                    data.meshes.push_back(std::move(ref));
                };
                for (const auto& visual : link.visuals)
                    add_mesh(visual.geometry);
                for (const auto& collision : link.collisions)
                    add_mesh(collision.geometry);
            }
        }

        // Render-only metadata: things the MuJoCo importer drops on purpose but the renderer needs
        // (URDF limit.velocity is used by TAA/animation) plus the link -> visual mapping.
        {
            nlohmann::json render;
            render["joint_velocity_limits"] = nlohmann::json::object();
            for (const auto& joint : robot_def.joints)
                render["joint_velocity_limits"][joint.name] = joint.limit.velocity;
            render["links"] = nlohmann::json::array();
            for (const auto& link : robot_def.links) {
                nlohmann::json entry;
                entry["name"] = link.name;
                entry["visuals"] = nlohmann::json::array();
                for (const auto& visual : link.visuals) {
                    nlohmann::json visual_entry;
                    visual_entry["mesh_path"] = visual.geometry.mesh_path;
                    visual_entry["color_rgba"] = {
                        visual.color_rgba[0], visual.color_rgba[1], visual.color_rgba[2], visual.color_rgba[3]
                    };
                    entry["visuals"].push_back(std::move(visual_entry));
                }
                render["links"].push_back(std::move(entry));
            }
            data.render_metadata_json = render.dump();
        }

        // Physics metadata: the sim-to-real parameters and the solver/contact choices, so the
        // runtime (and later the GPU XPBD backend) can read them back without re-deriving.
        {
            nlohmann::json physics;
            physics["timestep"] = options.timestep;
            physics["solver_iterations"] = options.solver_iterations;
            physics["contact_noslip"] = options.contact_noslip;
            physics["max_hull_vertices"] = options.max_hull_vertices;
            physics["joint_groups"] = nlohmann::json::array();
            for (const auto& group : options.joint_groups) {
                nlohmann::json entry;
                entry["token"] = group.token;
                entry["kp"] = group.kp;
                entry["kv"] = group.kv;
                entry["armature"] = group.armature;
                entry["max_torque"] = group.max_torque;
                physics["joint_groups"].push_back(std::move(entry));
            }
            data.physics_metadata_json = physics.dump();
        }

        support::log_info("[MujocoCook] cooked '" + robot_def.name + "': bodies=" +
                          std::to_string(model->nbody) + " joints=" + std::to_string(model->njnt) +
                          " geoms=" + std::to_string(model->ngeom) + " meshes=" +
                          std::to_string(model->nmesh) + " actuators=" + std::to_string(model->nu) +
                          " payload_bytes=" + std::to_string(data.model_payload.size()) +
                          " (" + (data.format == bud::robots::MujocoModelFormat::Mjcf ? "MJCF" : "MJB") + ")");

        mj_deleteModel(model);
        mj_deleteSpec(spec);
        mj_deleteVFS(&vfs);
        return data;
    }

} // namespace bud::asset_pipeline
