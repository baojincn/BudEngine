// Standalone MuJoCo spike.
//
// The question: can MuJoCo compile the G1 URDF we already have, straight from memory (mjVFS), and
// does the robot stand on a plane driven only by position actuators (kp/kv per joint group)?
//
// No engine, no Jolt. This is the feasibility check before making MuJoCo the only handler of robot
// data: URDF parsing, inertials, joints, collision hulls, motors and contacts all come from MuJoCo.
// The VFS is the same mechanism the engine will use to feed MuJoCo out of .budasset chunks
// (mj_addBufferVFS), so nothing here depends on loose files being shipped next to the model.
//
// Usage: mujoco_spike [urdf_path] [package_root]

#include <mujoco/mujoco.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace {

    struct JointGains {
        const char* token;
        double kp;        // N*m/rad
        double kv;        // N*m/(rad/s)
        double max_torque;// N*m
    };

    // G1 class gains and URDF effort limits. armature/frictionloss are not in the URDF at all and
    // would be added by our project config (point-identified for sim-to-real).
    const JointGains k_gains[] = {
        { "knee",     300.0, 15.0, 139.0 },
        { "ankle",     40.0,  2.0,  35.0 },
        { "hip",      200.0, 10.0,  88.0 },
        { "waist",    200.0, 10.0,  88.0 },
        { "shoulder",  60.0,  3.0,  25.0 },
        { "elbow",     40.0,  2.0,  25.0 },
        { "wrist",     40.0,  2.0,  25.0 },
    };

    void gains_for(const std::string& name, double& kp, double& kv, double& max_torque) {
        kp = 40.0;
        kv = 2.0;
        max_torque = 25.0;
        for (const JointGains& g : k_gains) {
            if (name.find(g.token) != std::string::npos) {
                kp = g.kp;
                kv = g.kv;
                max_torque = g.max_torque;
                return;
            }
        }
    }

    // Body local +Z expressed in world, from the body quaternion (w, x, y, z).
    void body_up_axis(const double* quat, double out_up[3]) {
        const double w = quat[0];
        const double x = quat[1];
        const double y = quat[2];
        const double z = quat[3];
        // rotate (0, 0, 1) by q
        const double tx = 2.0 * (y * 1.0);
        const double ty = 2.0 * (-x * 1.0);
        const double tz = 0.0;
        out_up[0] = x * tz - 0.0 + w * tx + (y * 0.0 - z * ty);
        out_up[1] = y * tz - 0.0 + w * ty + (z * tx - x * 0.0);
        out_up[2] = 1.0 + w * tz + (x * ty - y * tx);
    }

} // namespace

int main(int argc, char** argv) {
    // Unbuffered: a crash must not swallow the progress log (stdout to a pipe is fully buffered).
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool selftest = argc > 1 && std::string(argv[1]) == "--selftest";
    const std::filesystem::path package_root =
        argc > 2 ? std::filesystem::path(argv[2])
                 : std::filesystem::path("D:/PersonalProjects/unitree_ros-master/robots/g1_description");
    const std::string urdf_name =
        argc > 1 ? std::filesystem::path(argv[1]).filename().string() : std::string("g1_29dof.urdf");
    // An absolute path is taken as-is (handy for reduced variants placed outside the package), a
    // bare name is looked up inside package_root.
    const std::filesystem::path urdf_path =
        (argc > 1 && std::filesystem::path(argv[1]).is_absolute()) ? std::filesystem::path(argv[1])
                                                                   : package_root / urdf_name;

    if (!selftest && !std::filesystem::exists(urdf_path)) {
        std::printf("[spike] URDF not found: %s\n", urdf_path.string().c_str());
        return 1;
    }

    // Everything is served from an in-memory VFS.
    mjVFS vfs;
    mj_defaultVFS(&vfs);
    // Accept either a file name inside package_root or an absolute path (handy for reduced
    // variants used to bisect parser problems); either way the VFS entry uses the bare file name.
    const std::string vfs_urdf_name = urdf_path.filename().string();
    mj_addFileVFS(&vfs, urdf_path.parent_path().string().c_str(), urdf_path.filename().string().c_str());

    size_t mesh_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(package_root)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".stl" && ext != ".obj")
            continue;
        // MuJoCo resolves mesh files by the path written in the URDF ("meshes/x.STL"), so the VFS
        // entry must keep exactly that relative spelling.
        const std::string relative = std::filesystem::relative(entry.path(), package_root).generic_string();
        if (mj_addFileVFS(&vfs, package_root.string().c_str(), relative.c_str()) == 0)
            ++mesh_count;
    }
    std::printf("[spike] VFS ready: urdf=%s, meshes=%zu\n", urdf_name.c_str(), mesh_count);

    char error[2048] = { 0 };

    // Self test first: if even a trivial MJCF/URDF string fails, the parser or the linkage is at
    // fault rather than our model.
    if (selftest) {
        const char* minimal_mjcf =
            "<mujoco model='t'><worldbody><body name='b' pos='0 0 0.5'>"
            "<freejoint/><geom type='box' size='0.1 0.1 0.1'/></body></worldbody></mujoco>";
        mjSpec* mjcf_spec = mj_parseXMLString(minimal_mjcf, &vfs, error, sizeof(error));
        std::printf("[selftest] minimal MJCF: %s (%s)\n", mjcf_spec ? "OK" : "FAILED",
                    mjcf_spec ? "" : error);
        if (mjcf_spec)
            mj_deleteSpec(mjcf_spec);

        const char* minimal_urdf =
            "<robot name='t'><link name='base'><inertial><mass value='1'/>"
            "<inertia ixx='0.01' iyy='0.01' izz='0.01' ixy='0' ixz='0' iyz='0'/></inertial>"
            "</link></robot>";
        mjSpec* urdf_spec = mj_parseXMLString(minimal_urdf, &vfs, error, sizeof(error));
        std::printf("[selftest] minimal URDF: %s (%s)\n", urdf_spec ? "OK" : "FAILED",
                    urdf_spec ? "" : error);
        if (urdf_spec)
            mj_deleteSpec(urdf_spec);
        return 0;
    }

    std::printf("[spike] parsing %s ...\n", vfs_urdf_name.c_str());
    mjSpec* spec = mj_parseXML(vfs_urdf_name.c_str(), &vfs, error, sizeof(error));
    if (!spec) {
        std::printf("[spike] parse failed: %s\n", error);
        return 1;
    }
    std::printf("[spike] parse OK\n");

    // MuJoCo and the URDF are both Z-up, so no axis conversion is needed here. (Our engine is
    // Y-up; the integration layer owns that rotation.)
    mjsBody* world = mjs_findBody(spec, "world");
    std::printf("[spike] phase: floor (world=%s)\n", world ? "found" : "NULL");
    if (world) {
        mjsGeom* floor_geom = mjs_addGeom(world, nullptr);
        std::printf("[spike]   floor geom added\n");
        mjs_setName(floor_geom->element, "floor");
        std::printf("[spike]   floor named\n");
        floor_geom->type = mjGEOM_PLANE;
        floor_geom->size[0] = 20.0;
        floor_geom->size[1] = 20.0;
        floor_geom->size[2] = 0.05; // plane "spacing" (render only) must be positive
        floor_geom->rgba[0] = 0.35;
        floor_geom->rgba[1] = 0.35;
        floor_geom->rgba[2] = 0.4;
        floor_geom->rgba[3] = 1.0;
    }

    // Cook step normalization #1: the URDF base is "world --floating--> pelvis", and MuJoCo's
    // importer drops that joint, which welds the robot to the world (free=0, and every "standing"
    // result is then meaningless). The free joint has to be added explicitly. This is exactly the
    // kind of fixup the offline cook step owns - the runtime should never see a raw URDF.
    mjsBody* pelvis = mjs_findBody(spec, "pelvis");
    std::printf("[spike] pelvis body: %s\n", pelvis ? "found" : "MISSING");
    if (pelvis) {
        if (mjs_addFreeJoint(pelvis) == nullptr)
            std::printf("[spike] WARNING: could not add a free joint to the pelvis\n");
    }

    std::printf("[spike] phase: first compile (no actuators)\n");
    mjModel* model = mj_compile(spec, &vfs);
    if (!model) {
        const char* compile_error = mjs_getError(spec);
        std::printf("[spike] compile failed: %s\n", compile_error ? compile_error : "(no message)");
        return 1;
    }

    // Critical: is the robot actually free to move? A URDF "world" link plus a floating base joint
    // must turn into a body with a FREE joint. If it does not, the robot is welded to the world and
    // any apparent "standing" is meaningless (it would just be pinned, like a kinematic pelvis).
    int free_joints = 0;
    int hinge_joint_count = 0;
    for (int joint = 0; joint < model->njnt; ++joint) {
        if (model->jnt_type[joint] == mjJNT_FREE)
            ++free_joints;
        else if (model->jnt_type[joint] == mjJNT_HINGE)
            ++hinge_joint_count;
    }
    std::printf("[spike] joints: free=%d hinge=%d (of %d)\n", free_joints, hinge_joint_count,
                static_cast<int>(model->njnt));
    for (int body = 0; body < model->nbody && body < 6; ++body) {
        const char* body_name = mj_id2name(model, mjOBJ_BODY, body);
        std::printf("[spike]   body %d '%s' mass=%.3f\n", body, body_name ? body_name : "?",
                    model->body_mass[body]);
    }

    double total_mass = 0.0;
    for (int body = 1; body < model->nbody; ++body)
        total_mass += model->body_mass[body];
    std::printf("[spike] model: nbody=%d njnt=%d ngeom=%d nmesh=%d mass=%.2f kg\n",
                static_cast<int>(model->nbody), static_cast<int>(model->njnt),
                static_cast<int>(model->ngeom), static_cast<int>(model->nmesh), total_mass);

    // List the hinge joints MuJoCo produced from the URDF. This is our joint-name source of truth:
    // the real backend takes the same names from the robot asset metadata.
    struct ActuatorTarget {
        std::string joint;
        double kp;
        double kv;
        double max_torque;
    };
    std::vector<ActuatorTarget> targets;
    int hinge_joints = 0;
    for (int joint = 0; joint < model->njnt; ++joint) {
        if (model->jnt_type[joint] != mjJNT_HINGE)
            continue;
        ++hinge_joints;
        const char* joint_name = mj_id2name(model, mjOBJ_JOINT, joint);
        if (!joint_name)
            continue;
        if (hinge_joints <= 6)
            std::printf("[spike]   hinge joint: %s\n", joint_name);

        double kp = 0.0;
        double kv = 0.0;
        double max_torque = 0.0;
        gains_for(joint_name, kp, kv, max_torque);

        mjsElement* joint_element = mjs_findElement(spec, mjOBJ_JOINT, joint_name);
        if (!joint_element) {
            std::printf("[spike]   joint '%s' not found in spec\n", joint_name);
            continue;
        }

        mjsActuator* actuator = mjs_addActuator(spec, nullptr);
        mjs_setName(actuator->element, joint_name);
        actuator->trntype = mjTRN_JOINT;
        if (actuator->target != nullptr)
            mjs_setString(actuator->target, joint_name);
        double kv_array[1] = { kv };
        // kv and dampratio are mutually exclusive; passing a non-null pointer counts as "defined".
        // On success MuJoCo returns a pointer to an *empty* string, so an empty message is success.
        const char* position_error =
            mjs_setToPosition(actuator, kp, kv_array, nullptr, nullptr, 0.0);
        if (position_error != nullptr && position_error[0] != '\0') {
            std::printf("[spike]   actuator '%s' setup failed: %s\n", joint_name, position_error);
            continue;
        }
        actuator->forcelimited = 1;
        actuator->forcerange[0] = -max_torque;
        actuator->forcerange[1] = max_torque;
        targets.push_back({ joint_name, kp, kv, max_torque });
    }
    std::printf("[spike] hinge joints=%d, actuators added=%zu\n", hinge_joints, targets.size());

    // Recompile with the actuators attached.
    mj_deleteModel(model);
    std::printf("[spike] phase: second compile (with actuators)\n");
    model = mj_compile(spec, &vfs);
    if (!model) {
        const char* compile_error = mjs_getError(spec);
        std::printf("[spike] recompile failed: %s\n", compile_error ? compile_error : "(no message)");
        return 1;
    }
    std::printf("[spike] model: nu=%d\n", static_cast<int>(model->nu));
    std::printf("[spike] timestep=%.4f (MuJoCo default integrator)\n", model->opt.timestep);

    mjData* data = mj_makeData(model);

    // Place the robot at a standing height before stepping: the free joint's qpos defaults to the
    // origin, which for this URDF puts the pelvis at z=0 with the feet below the floor.
    int base_joint = -1;
    for (int joint = 0; joint < model->njnt; ++joint) {
        if (model->jnt_type[joint] == mjJNT_FREE) {
            base_joint = joint;
            break;
        }
    }
    constexpr double kStandingPelvisHeight = 0.79;
    if (base_joint >= 0) {
        const int qpos_adr = model->jnt_qposadr[base_joint];
        data->qpos[qpos_adr + 0] = 0.0;
        data->qpos[qpos_adr + 1] = 0.0;
        data->qpos[qpos_adr + 2] = kStandingPelvisHeight;
        data->qpos[qpos_adr + 3] = 1.0; // quaternion w
        data->qpos[qpos_adr + 4] = 0.0;
        data->qpos[qpos_adr + 5] = 0.0;
        data->qpos[qpos_adr + 6] = 0.0;
        mj_forward(model, data);
        std::printf("[spike] base placed at z=%.2f\n", kStandingPelvisHeight);
    }

    // Hold the spawn pose: each position actuator targets the joint angle the model was built with.
    for (int actuator = 0; actuator < model->nu; ++actuator) {
        const int joint_id = model->actuator_trnid[2 * actuator];
        data->ctrl[actuator] = joint_id >= 0 ? data->qpos[model->jnt_qposadr[joint_id]] : 0.0;
    }

    const int root_body = 1; // body 0 is the world

    // Where is the whole body CoM relative to the foot support centre at the start? A posture
    // controller cannot fix a CoM that starts outside the support polygon; this number is the
    // balance controller's first target.
    {
        const int left_foot = mj_name2id(model, mjOBJ_BODY, "left_ankle_roll_link");
        const int right_foot = mj_name2id(model, mjOBJ_BODY, "right_ankle_roll_link");
        if (left_foot > 0 && right_foot > 0) {
            const double support_x = 0.5 * (data->xpos[3 * left_foot + 0] + data->xpos[3 * right_foot + 0]);
            const double support_y = 0.5 * (data->xpos[3 * left_foot + 1] + data->xpos[3 * right_foot + 1]);
            const double* com = data->subtree_com; // whole-robot CoM at the world body
            std::printf("[spike] CoM=(%.4f, %.4f, %.4f) support centre=(%.4f, %.4f) offset=(%.4f, %.4f)\n",
                        com[0], com[1], com[2], support_x, support_y,
                        com[0] - support_x, com[1] - support_y);
        }
    }
    const double duration = 5.0;
    const int steps = static_cast<int>(duration / model->opt.timestep);
    double min_height = 1.0e9;
    double max_tilt_deg = 0.0;
    int print_every = steps / 20;

    // Lateral disturbance at t = 1.5 s: 60 N for 0.1 s applied to the pelvis. A posture-only
    // controller (what this spike uses) is expected to survive a push only if the CoM stays inside
    // the foot support; recovering from a real push is the balance controller's job.
    constexpr int kPushStep = 750;             // 1.5 s
    constexpr int kPushSteps = 50;             // 0.1 s
    constexpr double kPushForce = 60.0;        // N
    double pushed_height = 0.0;

    for (int step = 0; step <= steps; ++step) {
        if (step > 0) {
            if (step >= kPushStep && step < kPushStep + kPushSteps)
                data->xfrc_applied[6 * root_body + 0] = kPushForce;
            else
                data->xfrc_applied[6 * root_body + 0] = 0.0;
            mj_step(model, data);
        }

        const double* position = data->xpos + 3 * root_body;
        double up[3] = { 0.0, 0.0, 1.0 };
        body_up_axis(data->xquat + 4 * root_body, up);
        const double tilt_deg = std::acos(std::clamp(up[2], -1.0, 1.0)) * 180.0 / 3.14159265358979;

        min_height = std::min(min_height, position[2]);
        max_tilt_deg = std::max(max_tilt_deg, tilt_deg);
        if (step == kPushStep + kPushSteps)
            pushed_height = position[2];

        if (print_every <= 0 || step % print_every == 0) {
            std::printf("[spike] t=%.2fs  root_y=%.4f  tilt=%.2f deg\n",
                        step * model->opt.timestep, position[2], tilt_deg);
        }
    }

    const double* final_position = data->xpos + 3 * root_body;
    double lowest_z = 1.0e9;
    for (int body = 1; body < model->nbody; ++body)
        lowest_z = std::min(lowest_z, data->xpos[3 * body + 2]);
    std::printf("[spike] final: root_z=%.4f, lowest_body_z=%.4f, max_tilt=%.2f deg\n",
                final_position[2], lowest_z, max_tilt_deg);
    for (int body = 1; body <= 3 && body < model->nbody; ++body) {
        const char* body_name = mj_id2name(model, mjOBJ_BODY, body);
        std::printf("[spike]   body %d '%s' z=%.4f\n", body, body_name ? body_name : "?",
                    data->xpos[3 * body + 2]);
    }
    // Standing = the pose is held (root height essentially unchanged) and roughly upright. No
    // balance controller is running here, so a small limit cycle is expected; falling is not.
    const bool height_held = std::fabs(final_position[2] - kStandingPelvisHeight) < 0.15;
    std::printf("[spike] push response: z at end of push=%.4f (start 0.79)\n", pushed_height);
    std::printf("[spike] VERDICT: %s\n",
                (max_tilt_deg < 25.0 && height_held)
                    ? "STANDS (holds the commanded pose; no balance controller)"
                    : "FALLS under pure position control");

    mj_deleteData(data);
    mj_deleteModel(model);
    mj_deleteSpec(spec);
    return 0;
}
