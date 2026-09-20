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

        // MuJoCo loads STL/OBJ (not DAE), so only those are registered in the cook VFS.
        bool is_mujoco_mesh(const fs::path& path) {
            const std::string ext = lower_extension(path);
            return ext == ".stl" || ext == ".obj";
        }

        // Passive motor parameters a URDF cannot express (rotor inertia, damping, Coulomb friction).
        // The values are copied from Unitree's own MJCF (unitree_mujoco g1_29dof.xml) rather than
        // invented here: every joint uses armature 0.01 / damping 0.05 / frictionloss 0.2, and the
        // wrist joints use a lower friction loss of 0.1.
        struct JointMotorParams {
            double armature = 0.01;
            double damping = 0.05;
            double frictionloss = 0.2;
        };

        JointMotorParams joint_motor_params_for(const std::string& joint_name) {
            JointMotorParams params;
            if (joint_name.find("wrist") != std::string::npos)
                params.frictionloss = 0.1;
            return params;
        }

        // Torque limits where the URDF snapshot and Unitree's own MJCF disagree. The official
        // g1_29dof.xml simulates the ankle and waist roll/pitch joints at +-50 Nm while the URDF in
        // unitree_ros still says 35, so the official value wins here: the MJCF is what Unitree
        // actually simulates with. Every other joint already matches (hip 88, knee 139, waist_yaw 88,
        // shoulder/elbow/wrist_roll 25, wrist_pitch/yaw 5).
        double joint_torque_limit_for(const std::string& joint_name, double urdf_effort) {
            if (joint_name.find("ankle") != std::string::npos)
                return 50.0;
            if (joint_name.find("waist") != std::string::npos &&
                joint_name.find("yaw") == std::string::npos)
                return 50.0;
            return urdf_effort;
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

        // Normalization #2: per-joint motor model. A URDF has neither armature (rotor inertia), joint
        // damping nor Coulomb friction, and those are exactly the sim-to-real parameters. The values
        // are copied from Unitree's own MJCF (g1_29dof.xml) so the cooked model matches the official
        // one instead of inventing numbers. Actuators are torque motors, again matching the official
        // model; the PD gains are the controller's business and stay out of the asset.
        // Passive motor physics on the joints, plus one torque motor per actuated joint. The torque
        // limit is applied through ctrlrange, exactly like the official model's
        // "<motor ctrlrange='-88 88'>": for a motor actuator force = gear * ctrl, so ctrlrange is the
        // torque limit. It comes from the URDF effort value, except where the official MJCF overrides
        // it (see joint_torque_limit_for).
        {
            int passive_joints = 0;
            int motors_created = 0;
            for (const auto& joint : robot_def.joints) {
                if (joint.type == bud::robots::JointType::Fixed)
                    continue;

                mjsElement* joint_element = mjs_findElement(spec, mjOBJ_JOINT, joint.name.c_str());
                mjsJoint* mj_joint = joint_element ? mjs_asJoint(joint_element) : nullptr;
                if (!mj_joint) {
                    support::log_warn("[MujocoCook] joint '" + joint.name +
                                      "' not found in the spec; skipping its motor model");
                    continue;
                }

                const JointMotorParams motor_params = joint_motor_params_for(joint.name);
                mj_joint->armature = motor_params.armature;
                mj_joint->damping = motor_params.damping;
                mj_joint->frictionloss = motor_params.frictionloss;
                ++passive_joints;

                mjsActuator* actuator = mjs_addActuator(spec, nullptr);
                if (!actuator) {
                    support::log_warn("[MujocoCook] could not create a motor for joint '" + joint.name + "'");
                    continue;
                }
                const std::string actuator_name = joint.name + "_motor";
                mjs_setName(actuator->element, actuator_name.c_str());
                actuator->trntype = mjTRN_JOINT;
                if (actuator->target != nullptr)
                    mjs_setString(actuator->target, joint.name.c_str());
                mjs_setToMotor(actuator);
                actuator->gear[0] = 1.0;
                const double torque_limit =
                    joint_torque_limit_for(joint.name, static_cast<double>(joint.limit.effort));
                if (torque_limit > 0.0) {
                    actuator->ctrllimited = 1;
                    actuator->ctrlrange[0] = -torque_limit;
                    actuator->ctrlrange[1] = torque_limit;
                }
                ++motors_created;
            }
            support::log_info("[MujocoCook] motor model: " + std::to_string(passive_joints) +
                              " joints with armature/damping/frictionloss, " +
                              std::to_string(motors_created) + " torque motors");
        }

        // IMU on the root link. RL observations want base orientation and angular velocity in the
        // body frame; MuJoCo's own sensor pipeline gives exactly that, so the runtime reads
        // sensordata instead of reconstructing it from qpos/qvel.
        {
            if (mjs_findElement(spec, mjOBJ_SITE, "imu") != nullptr) {
                support::log_info("[MujocoCook] a site named 'imu' already exists; keeping it");
            } else {
                mjsBody* root_body = mjs_findBody(spec, robot_def.root_link.c_str());
                if (root_body == nullptr) {
                    support::log_warn("[MujocoCook] root link '" + robot_def.root_link +
                                      "' not found; no IMU site was added");
                } else {
                    mjsSite* imu_site = mjs_addSite(root_body, nullptr);
                    mjs_setName(imu_site->element, "imu");

                    const struct {
                        mjtSensor type;
                        const char* name;
                    } imu_sensors[] = {
                        { mjSENS_FRAMEQUAT, "imu_quat" },
                        { mjSENS_GYRO, "imu_gyro" },
                        { mjSENS_ACCELEROMETER, "imu_accel" },
                    };
                    int imu_sensor_count = 0;
                    for (const auto& entry : imu_sensors) {
                        mjsSensor* sensor = mjs_addSensor(spec);
                        if (sensor == nullptr)
                            continue;
                        sensor->type = entry.type;
                        sensor->objtype = mjOBJ_SITE;
                        if (sensor->objname != nullptr)
                            mjs_setString(sensor->objname, "imu");
                        mjs_setName(sensor->element, entry.name);
                        ++imu_sensor_count;
                    }
                    support::log_info("[MujocoCook] IMU on root link '" + robot_def.root_link + "': " +
                                      std::to_string(imu_sensor_count) +
                                      " sensors (framequat/gyro/accelerometer)");
                }
            }
        }

        // Foot contact needs torsional friction. MuJoCo's URDF import leaves geoms at the default
        // condim (3 = sliding only), and a foot with no torsional friction spins freely on the ground
        // when a walking policy plants it, so the robot veers off its commanded heading even though it
        // steps. Raising condim to 4 enables the spin term on the ankle-roll (foot) geoms.
        {
            int foot_geoms = 0;
            for (mjsElement* element = mjs_firstElement(spec, mjOBJ_GEOM); element != nullptr;
                 element = mjs_nextElement(spec, element)) {
                mjsGeom* geom = mjs_asGeom(element);
                if (geom == nullptr)
                    continue;
                mjsBody* body = mjs_getParent(element);
                if (body == nullptr)
                    continue;
                const char* body_name = mjs_getString(mjs_getName(body->element));
                if (body_name == nullptr)
                    continue;
                if (std::string(body_name).find("ankle_roll") == std::string::npos)
                    continue;
                geom->condim = 4;
                geom->friction[1] = 0.5; // torsional (spin)
                ++foot_geoms;
            }
            support::log_info("[MujocoCook] foot contact: condim=4 + torsional friction on " +
                              std::to_string(foot_geoms) + " geom(s)");
        }

        support::log_info("[MujocoCook] phase: compile");
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

        // Conformance census: the cooked model must match the official MJCF value for value. These
        // lines are the comparison surface against unitree_mujoco's g1_29dof.xml, so any drift in the
        // URDF -> MJCF conversion (armature, friction loss, torque limits, masses) is visible here.
        {
            double total_mass = 0.0;
            for (int body = 1; body < model->nbody; ++body)
                total_mass += model->body_mass[body];
            support::log_info(
                "[MujocoCook][conformance] counts: nbody=" + std::to_string(model->nbody) +
                " njnt=" + std::to_string(model->njnt) + " nu=" + std::to_string(model->nu) +
                " ngeom=" + std::to_string(model->ngeom) + " nmesh=" + std::to_string(model->nmesh) +
                " nsensor=" + std::to_string(model->nsensor) +
                " mass=" + std::to_string(total_mass));

            for (int joint = 0; joint < model->njnt; ++joint) {
                const int joint_type = model->jnt_type[joint];
                if (joint_type != mjJNT_HINGE && joint_type != mjJNT_SLIDE)
                    continue;
                const int dof = model->jnt_dofadr[joint];
                if (dof < 0)
                    continue;

                double torque_limit = 0.0;
                for (int actuator = 0; actuator < model->nu; ++actuator) {
                    if (model->actuator_trnid[2 * actuator] == joint) {
                        torque_limit = model->actuator_ctrlrange[2 * actuator + 1];
                        break;
                    }
                }
                const char* joint_name = mj_id2name(model, mjOBJ_JOINT, joint);
                support::log_info(
                    std::string("[MujocoCook][conformance] joint '") + (joint_name ? joint_name : "?") +
                    "' armature=" + std::to_string(model->dof_armature[dof]) +
                    " damping=" + std::to_string(model->dof_damping[dof]) +
                    " frictionloss=" + std::to_string(model->dof_frictionloss[dof]) +
                    " torque_limit=" + std::to_string(torque_limit));
            }
        }

        // Save the normalized model: the runtime compiles this, so the runtime never sees the URDF.
        // MJCF text is preferred (readable, version independent, meshes stay logical references).
        // MuJoCo's XML writers are picky about spec-built models, so fall back to the compiled MJB
        // binary, which works for any model and embeds the collision mesh data.

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

        // No hull post-processing: the collision hulls come from Unitree's own meshes with MuJoCo's
        // own hull builder, so we do not inject a vertex cap or any other number into the model.

        // Mesh references: the model asks for meshes by "meshdir + file", so the keys are taken from
        // the MJCF text itself (our asset paths use a different layout, e.g. meshes/visual/x.budasset)
        // and each is mapped to the cooked asset with the same stem. Meshes stay logical references:
        // the runtime rebuilds the STL bytes from the RawMesh chunk already in the package.
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

            std::string mesh_dir = "meshes/";
            const std::string compiler_tag = "<compiler ";
            const size_t compiler_at = data.model_payload.find(compiler_tag);
            if (compiler_at != std::string::npos) {
                const size_t meshdir_at = data.model_payload.find("meshdir=\"", compiler_at);
                if (meshdir_at != std::string::npos) {
                    const size_t value_at = meshdir_at + 9;
                    const size_t value_end = data.model_payload.find('"', value_at);
                    if (value_end != std::string::npos)
                        mesh_dir = data.model_payload.substr(value_at, value_end - value_at);
                }
            }

            const std::string mesh_tag = "<mesh ";
            size_t cursor = 0;
            while (true) {
                const size_t at = data.model_payload.find(mesh_tag, cursor);
                if (at == std::string::npos)
                    break;
                const size_t file_at = data.model_payload.find("file=\"", at);
                if (file_at == std::string::npos || file_at > data.model_payload.find('>', at))
                    break;
                const size_t value_at = file_at + 6;
                const size_t value_end = data.model_payload.find('"', value_at);
                if (value_end == std::string::npos)
                    break;
                cursor = value_end + 1;

                bud::robots::MujocoMeshRef ref{};
                const std::string file_name = data.model_payload.substr(value_at, value_end - value_at);
                ref.mjcf_name = mesh_dir + file_name;
                const auto it = stem_to_asset.find(file_stem_lower(fs::path(file_name)));
                if (it == stem_to_asset.end()) {
                    support::log_warn("[MujocoCook] No cooked visual asset for mesh '" + file_name + "'");
                    continue;
                }
                ref.asset_path = it->second;
                data.meshes.push_back(std::move(ref));
            }
            support::log_info("[MujocoCook] mesh references: " + std::to_string(data.meshes.size()) +
                              " (meshdir='" + mesh_dir + "')");
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
        // No physics metadata is baked: solver settings (timestep, iterations, contact model) are
        // experiment configuration and belong to the runtime, not to the robot asset.

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
