#include "mjcf_parser.hpp"
#include "src/tools/asset_pipeline/third_party/pugixml/pugixml.hpp"

#include <iostream>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

namespace bud::asset_pipeline {

namespace {

std::string normalize_slash(const std::string& path_str) {
    std::string res = path_str;
    std::replace(res.begin(), res.end(), '\\', '/');
    return res;
}

void parse_float_array(const char* str, float* out_arr, size_t count) {
    if (!str)
        return;

    std::stringstream ss(str);
    for (size_t i = 0; i < count; ++i) {
        if (!(ss >> out_arr[i]))
            break;
    }
}

glm::vec3 quat_to_rpy(float w, float x, float y, float z) {
    float m00 = 1.0f - 2.0f * (y * y + z * z);
    float m10 = 2.0f * (x * y + z * w);
    float m20 = 2.0f * (x * z - y * w);
    float m21 = 2.0f * (y * z + x * w);
    float m22 = 1.0f - 2.0f * (x * x + y * y);

    float sin_p = std::clamp(-m20, -1.0f, 1.0f);
    float pitch = std::asin(sin_p);
    float cos_p = std::cos(pitch);

    float roll = 0.0f;
    float yaw = 0.0f;
    if (std::abs(cos_p) > 1e-6f) {
        roll = std::atan2(m21, m22);
        yaw = std::atan2(m10, m00);
    } else {
        float m11 = 1.0f - 2.0f * (x * x + z * z);
        float m12 = 2.0f * (y * z - x * w);
        roll = std::atan2(-m12, m11);
        yaw = 0.0f;
    }
    return glm::vec3(roll, pitch, yaw);
}

struct ActuatorDefaults {
    float damping = 0.0f;
    float frictionloss = 0.0f;
    float armature = 0.0f;
    float kp = 0.0f;
    float kv = 0.0f;
    float min_force = 0.0f;
    float max_force = 0.0f;
};

void parse_defaults_recursive(
    const pugi::xml_node& default_node,
    const std::string& parent_class,
    std::unordered_map<std::string, ActuatorDefaults>& class_defaults
) {
    std::string current_class = default_node.attribute("class").as_string(parent_class.c_str());
    ActuatorDefaults def{};
    if (!parent_class.empty() && class_defaults.count(parent_class) > 0)
        def = class_defaults[parent_class];

    pugi::xml_node joint_node = default_node.child("joint");
    if (joint_node) {
        if (joint_node.attribute("damping"))
            def.damping = joint_node.attribute("damping").as_float(def.damping);
        if (joint_node.attribute("frictionloss"))
            def.frictionloss = joint_node.attribute("frictionloss").as_float(def.frictionloss);
        if (joint_node.attribute("armature"))
            def.armature = joint_node.attribute("armature").as_float(def.armature);
    }

    pugi::xml_node pos_node = default_node.child("position");
    if (pos_node) {
        if (pos_node.attribute("kp"))
            def.kp = pos_node.attribute("kp").as_float(def.kp);
        if (pos_node.attribute("kv"))
            def.kv = pos_node.attribute("kv").as_float(def.kv);
        if (pos_node.attribute("forcerange")) {
            float fr[2] = { 0.0f, 0.0f };
            parse_float_array(pos_node.attribute("forcerange").as_string(), fr, 2);
            def.min_force = fr[0];
            def.max_force = fr[1];
        }
    }

    if (!current_class.empty())
        class_defaults[current_class] = def;

    for (pugi::xml_node child = default_node.child("default"); child; child = child.next_sibling("default"))
        parse_defaults_recursive(child, current_class, class_defaults);
}

void parse_body_recursive(
    const pugi::xml_node& body_node,
    const std::string& parent_link_name,
    const std::string& mesh_dir,
    const std::unordered_map<std::string, std::string>& mesh_assets,
    const std::unordered_map<std::string, std::vector<float>>& materials,
    const std::unordered_map<std::string, ActuatorDefaults>& class_defaults,
    bud::robot::RobotDef& robot_def
) {
    std::string link_name = body_node.attribute("name").as_string();
    if (link_name.empty())
        return;

    bud::robot::LinkDef link_def;
    link_def.name = link_name;

    float body_pos[3] = { 0.0f, 0.0f, 0.0f };
    float body_quat[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    parse_float_array(body_node.attribute("pos").as_string(), body_pos, 3);
    parse_float_array(body_node.attribute("quat").as_string(), body_quat, 4);
    glm::vec3 body_rpy = quat_to_rpy(body_quat[0], body_quat[1], body_quat[2], body_quat[3]);

    pugi::xml_node inertial_node = body_node.child("inertial");
    if (inertial_node) {
        parse_float_array(inertial_node.attribute("pos").as_string(), link_def.inertial.origin_xyz, 3);
        link_def.inertial.mass = inertial_node.attribute("mass").as_float(0.0f);

        if (inertial_node.attribute("fullinertia")) {
            // fullinertia = ixx iyy izz ixy ixz iyz
            float full_in[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            parse_float_array(inertial_node.attribute("fullinertia").as_string(), full_in, 6);
            link_def.inertial.inertia[0] = full_in[0]; // ixx
            link_def.inertial.inertia[1] = full_in[3]; // ixy
            link_def.inertial.inertia[2] = full_in[4]; // ixz
            link_def.inertial.inertia[3] = full_in[1]; // iyy
            link_def.inertial.inertia[4] = full_in[5]; // iyz
            link_def.inertial.inertia[5] = full_in[2]; // izz
        } else if (inertial_node.attribute("diaginertia")) {
            float diag_in[3] = { 0.0f, 0.0f, 0.0f };
            parse_float_array(inertial_node.attribute("diaginertia").as_string(), diag_in, 3);
            link_def.inertial.inertia[0] = diag_in[0];
            link_def.inertial.inertia[3] = diag_in[1];
            link_def.inertial.inertia[5] = diag_in[2];
        }
    }

    for (pugi::xml_node geom_node = body_node.child("geom"); geom_node; geom_node = geom_node.next_sibling("geom")) {
        std::string geom_type = geom_node.attribute("type").as_string("mesh");
        if (geom_type != "mesh")
            continue;

        std::string mesh_ref = geom_node.attribute("mesh").as_string();
        if (mesh_ref.empty())
            continue;

        std::string resolved_mesh_path;
        const auto it = mesh_assets.find(mesh_ref);
        if (it != mesh_assets.end())
            resolved_mesh_path = it->second;
        else {
            fs::path direct_candidate = fs::path(mesh_dir) / (mesh_ref + ".stl");
            if (fs::exists(direct_candidate))
                resolved_mesh_path = normalize_slash(direct_candidate.string());
        }

        if (resolved_mesh_path.empty())
            continue;

        float geom_pos[3] = { 0.0f, 0.0f, 0.0f };
        float geom_quat[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        parse_float_array(geom_node.attribute("pos").as_string(), geom_pos, 3);
        parse_float_array(geom_node.attribute("quat").as_string(), geom_quat, 4);
        glm::vec3 geom_rpy = quat_to_rpy(geom_quat[0], geom_quat[1], geom_quat[2], geom_quat[3]);

        std::string geom_class = geom_node.attribute("class").as_string();
        int contype = geom_node.attribute("contype").as_int(1);
        int conaffinity = geom_node.attribute("conaffinity").as_int(1);
        int group = geom_node.attribute("group").as_int(0);

        bool is_visual = (geom_class == "visual" || (contype == 0 && conaffinity == 0) || group == 2);
        bool is_collision = (geom_class == "collision" || geom_class == "self_collision_only" || group == 3 || contype > 0);

        if (!is_visual && !is_collision) {
            is_visual = true;
            is_collision = true;
        }

        if (is_visual) {
            bud::robot::VisualDef vis_def;
            vis_def.name = mesh_ref;
            vis_def.origin_xyz[0] = geom_pos[0];
            vis_def.origin_xyz[1] = geom_pos[1];
            vis_def.origin_xyz[2] = geom_pos[2];
            vis_def.origin_rpy[0] = geom_rpy.x;
            vis_def.origin_rpy[1] = geom_rpy.y;
            vis_def.origin_rpy[2] = geom_rpy.z;
            vis_def.geometry.type = bud::robot::GeometryType::Mesh;
            vis_def.geometry.mesh_path = resolved_mesh_path;

            std::string mat_name = geom_node.attribute("material").as_string();
            if (!mat_name.empty()) {
                vis_def.material_name = mat_name;
                const auto m_it = materials.find(mat_name);
                if (m_it != materials.end() && m_it->second.size() >= 4) {
                    vis_def.color_rgba[0] = m_it->second[0];
                    vis_def.color_rgba[1] = m_it->second[1];
                    vis_def.color_rgba[2] = m_it->second[2];
                    vis_def.color_rgba[3] = m_it->second[3];
                }
            }
            link_def.visuals.push_back(std::move(vis_def));
        }

        if (is_collision) {
            bud::robot::CollisionDef col_def;
            col_def.name = mesh_ref;
            col_def.origin_xyz[0] = geom_pos[0];
            col_def.origin_xyz[1] = geom_pos[1];
            col_def.origin_xyz[2] = geom_pos[2];
            col_def.origin_rpy[0] = geom_rpy.x;
            col_def.origin_rpy[1] = geom_rpy.y;
            col_def.origin_rpy[2] = geom_rpy.z;
            col_def.geometry.type = bud::robot::GeometryType::Mesh;
            col_def.geometry.mesh_path = resolved_mesh_path;
            link_def.collisions.push_back(std::move(col_def));
        }
    }

    robot_def.links.push_back(std::move(link_def));

    for (pugi::xml_node joint_node = body_node.child("joint"); joint_node; joint_node = joint_node.next_sibling("joint")) {
        std::string j_type_str = joint_node.attribute("type").as_string("hinge");
        std::string j_name = joint_node.attribute("name").as_string();
        if (j_name.empty())
            continue;

        bud::robot::JointDef joint_def;
        joint_def.name = j_name;
        joint_def.parent_link = parent_link_name;
        joint_def.child_link = link_name;

        if (j_type_str == "free")
            joint_def.type = bud::robot::JointType::Floating;
        else if (j_type_str == "slide")
            joint_def.type = bud::robot::JointType::Prismatic;
        else
            joint_def.type = bud::robot::JointType::Revolute;

        joint_def.origin_xyz[0] = body_pos[0];
        joint_def.origin_xyz[1] = body_pos[1];
        joint_def.origin_xyz[2] = body_pos[2];
        joint_def.origin_rpy[0] = body_rpy.x;
        joint_def.origin_rpy[1] = body_rpy.y;
        joint_def.origin_rpy[2] = body_rpy.z;

        float axis[3] = { 0.0f, 0.0f, 1.0f };
        parse_float_array(joint_node.attribute("axis").as_string("0 0 1"), axis, 3);
        joint_def.axis[0] = axis[0];
        joint_def.axis[1] = axis[1];
        joint_def.axis[2] = axis[2];

        if (joint_node.attribute("range")) {
            float range[2] = { -3.14159f, 3.14159f };
            parse_float_array(joint_node.attribute("range").as_string(), range, 2);
            joint_def.limit.lower = range[0];
            joint_def.limit.upper = range[1];
        }

        std::string j_class = joint_node.attribute("class").as_string();
        if (!j_class.empty()) {
            const auto it = class_defaults.find(j_class);
            if (it != class_defaults.end()) {
                joint_def.dynamics.damping = it->second.damping;
                joint_def.dynamics.friction = it->second.frictionloss;
                joint_def.motor.enabled = true;
                joint_def.motor.stiffness = it->second.kp;
                joint_def.motor.damping = it->second.kv > 0.0f ? it->second.kv : it->second.damping;
                joint_def.motor.max_force = std::abs(it->second.max_force);
                joint_def.limit.effort = joint_def.motor.max_force;
            }
        }

        if (joint_node.attribute("damping"))
            joint_def.dynamics.damping = joint_node.attribute("damping").as_float(joint_def.dynamics.damping);
        if (joint_node.attribute("frictionloss"))
            joint_def.dynamics.friction = joint_node.attribute("frictionloss").as_float(joint_def.dynamics.friction);

        robot_def.joints.push_back(std::move(joint_def));
    }

    for (pugi::xml_node child_body = body_node.child("body"); child_body; child_body = child_body.next_sibling("body"))
        parse_body_recursive(child_body, link_name, mesh_dir, mesh_assets, materials, class_defaults, robot_def);
}

} // namespace

std::optional<bud::robot::RobotDef> MjcfParser::parse_file(const std::string& filepath, const MjcfParseOptions& options) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());
    if (!result) {
        std::cerr << "[MjcfParser] XML error parsing file " << filepath << ": " << result.description() << std::endl;
        return std::nullopt;
    }

    std::string base_dir;
    std::error_code ec;
    fs::path p = fs::path(filepath);
    if (p.has_parent_path())
        base_dir = normalize_slash(fs::absolute(p.parent_path(), ec).string());
    else
        base_dir = normalize_slash(fs::current_path(ec).string());

    pugi::xml_node root_node = doc.child("mujoco");
    if (!root_node) {
        std::cerr << "[MjcfParser] Root <mujoco> node not found in " << filepath << std::endl;
        return std::nullopt;
    }

    bud::robot::RobotDef robot_def;
    robot_def.name = root_node.attribute("model").as_string("microduck");

    std::string mesh_rel_dir = "assets";
    pugi::xml_node compiler_node = root_node.child("compiler");
    if (compiler_node && compiler_node.attribute("meshdir"))
        mesh_rel_dir = compiler_node.attribute("meshdir").as_string("assets");

    if (!options.mesh_dir_override.empty())
        mesh_rel_dir = options.mesh_dir_override;

    fs::path resolved_mesh_dir = fs::path(base_dir) / mesh_rel_dir;
    std::string mesh_dir_str = normalize_slash(resolved_mesh_dir.string());

    std::unordered_map<std::string, std::string> mesh_assets;
    std::unordered_map<std::string, std::vector<float>> materials;

    pugi::xml_node asset_node = root_node.child("asset");
    if (asset_node) {
        for (pugi::xml_node mesh_node = asset_node.child("mesh"); mesh_node; mesh_node = mesh_node.next_sibling("mesh")) {
            std::string file_str = mesh_node.attribute("file").as_string();
            if (file_str.empty())
                continue;

            std::string stem = fs::path(file_str).stem().string();
            std::string name_attr = mesh_node.attribute("name").as_string(stem.c_str());
            fs::path full_file = resolved_mesh_dir / file_str;

            std::string normalized_full = normalize_slash(full_file.string());
            mesh_assets[name_attr] = normalized_full;
            mesh_assets[stem] = normalized_full;
            mesh_assets[file_str] = normalized_full;
        }

        for (pugi::xml_node mat_node = asset_node.child("material"); mat_node; mat_node = mat_node.next_sibling("material")) {
            std::string mat_name = mat_node.attribute("name").as_string();
            if (mat_name.empty() || !mat_node.attribute("rgba"))
                continue;

            float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            parse_float_array(mat_node.attribute("rgba").as_string(), rgba, 4);
            materials[mat_name] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        }
    }

    std::unordered_map<std::string, ActuatorDefaults> class_defaults;
    for (pugi::xml_node def_node = root_node.child("default"); def_node; def_node = def_node.next_sibling("default"))
        parse_defaults_recursive(def_node, "", class_defaults);

    pugi::xml_node world_node = root_node.child("worldbody");
    if (!world_node) {
        std::cerr << "[MjcfParser] <worldbody> node not found in " << filepath << std::endl;
        return std::nullopt;
    }

    pugi::xml_node root_body = world_node.child("body");
    if (!root_body) {
        std::cerr << "[MjcfParser] No root <body> found in <worldbody>" << std::endl;
        return std::nullopt;
    }

    robot_def.root_link = root_body.attribute("name").as_string("trunk_base");
    parse_body_recursive(root_body, "world", mesh_dir_str, mesh_assets, materials, class_defaults, robot_def);

    pugi::xml_node actuator_node = root_node.child("actuator");
    if (actuator_node) {
        for (pugi::xml_node act = actuator_node.first_child(); act; act = act.next_sibling()) {
            std::string joint_ref = act.attribute("joint").as_string();
            if (joint_ref.empty())
                continue;

            bud::robot::JointDef* j_def = robot_def.find_joint(joint_ref);
            if (!j_def)
                continue;

            std::string act_class = act.attribute("class").as_string();
            if (!act_class.empty() && class_defaults.count(act_class) > 0) {
                const auto& d = class_defaults[act_class];
                j_def->motor.enabled = true;
                j_def->motor.stiffness = d.kp;
                j_def->motor.damping = d.kv > 0.0f ? d.kv : d.damping;
                j_def->motor.max_force = std::abs(d.max_force);
                j_def->limit.effort = j_def->motor.max_force;
            }

            if (act.attribute("kp"))
                j_def->motor.stiffness = act.attribute("kp").as_float(j_def->motor.stiffness);
            if (act.attribute("kv"))
                j_def->motor.damping = act.attribute("kv").as_float(j_def->motor.damping);
        }
    }

    return robot_def;
}

std::optional<bud::robot::RobotDef> MjcfParser::parse_string(
    const std::string& xml_content,
    const std::string& base_dir,
    const MjcfParseOptions& options
) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml_content.c_str());
    if (!result) {
        std::cerr << "[MjcfParser] XML error parsing string: " << result.description() << std::endl;
        return std::nullopt;
    }

    pugi::xml_node root_node = doc.child("mujoco");
    if (!root_node)
        return std::nullopt;

    bud::robot::RobotDef robot_def;
    robot_def.name = root_node.attribute("model").as_string("microduck");
    return robot_def;
}

} // namespace bud::asset_pipeline
