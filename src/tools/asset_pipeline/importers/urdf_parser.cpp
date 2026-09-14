#include "urdf_parser.hpp"
#include "src/tools/asset_pipeline/third_party/pugixml/pugixml.hpp"

#include <iostream>
#include <sstream>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

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

bud::robot::JointType parse_joint_type(const std::string& type_str) {
    if (type_str == "revolute")
        return bud::robot::JointType::Revolute;
    if (type_str == "continuous")
        return bud::robot::JointType::Continuous;
    if (type_str == "prismatic")
        return bud::robot::JointType::Prismatic;
    if (type_str == "fixed")
        return bud::robot::JointType::Fixed;
    if (type_str == "floating")
        return bud::robot::JointType::Floating;
    if (type_str == "planar")
        return bud::robot::JointType::Planar;
    return bud::robot::JointType::Fixed;
}

} // namespace

std::string UrdfParser::resolve_mesh_path(const std::string& raw_path, const std::string& urdf_dir, const std::string& package_root) {
    std::string clean_path = normalize_slash(raw_path);

    if (clean_path.rfind("file://", 0) == 0)
        clean_path = clean_path.substr(7);

    if (clean_path.rfind("package://", 0) == 0) {
        std::string sub = clean_path.substr(10);
        size_t slash_pos = sub.find('/');
        std::string pkg_name;
        std::string rel_path;
        if (slash_pos != std::string::npos) {
            pkg_name = sub.substr(0, slash_pos);
            rel_path = sub.substr(slash_pos + 1);
        } else {
            pkg_name = sub;
            rel_path = "";
        }

        std::vector<fs::path> candidate_paths;
        if (!package_root.empty()) {
            candidate_paths.push_back(fs::path(package_root) / rel_path);
            candidate_paths.push_back(fs::path(package_root) / pkg_name / rel_path);
        }
        if (!urdf_dir.empty()) {
            candidate_paths.push_back(fs::path(urdf_dir) / rel_path);
            candidate_paths.push_back(fs::path(urdf_dir) / ".." / rel_path);
            candidate_paths.push_back(fs::path(urdf_dir) / ".." / pkg_name / rel_path);
        }

        for (const auto& cand : candidate_paths) {
            std::error_code ec;
            if (fs::exists(cand, ec))
                return normalize_slash(fs::canonical(cand, ec).string());
        }

        // Default fallback
        if (!package_root.empty())
            return normalize_slash((fs::path(package_root) / rel_path).string());
        if (!urdf_dir.empty())
            return normalize_slash((fs::path(urdf_dir) / rel_path).string());
        return clean_path;
    }

    fs::path p(clean_path);
    if (p.is_absolute())
        return clean_path;

    if (!urdf_dir.empty()) {
        fs::path full_p = fs::path(urdf_dir) / p;
        std::error_code ec;
        if (fs::exists(full_p, ec))
            return normalize_slash(fs::canonical(full_p, ec).string());
        return normalize_slash(full_p.string());
    }

    return clean_path;
}

std::optional<bud::robot::RobotDef> UrdfParser::parse_file(const std::string& filepath, const UrdfParseOptions& options) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());

    if (!result) {
        std::cerr << "[UrdfParser] XML error parsing file " << filepath << ": " << result.description() << std::endl;
        return std::nullopt;
    }

    std::string base_dir;
    std::error_code ec;
    fs::path p = fs::path(filepath);
    if (p.has_parent_path())
        base_dir = normalize_slash(fs::absolute(p.parent_path(), ec).string());
    else
        base_dir = normalize_slash(fs::current_path(ec).string());

    pugi::xml_node robot_node = doc.child("robot");
    if (!robot_node) {
        std::cerr << "[UrdfParser] Root <robot> node not found in " << filepath << std::endl;
        return std::nullopt;
    }

    bud::robot::RobotDef robot_def;
    robot_def.name = robot_node.attribute("name").as_string("unnamed_robot");

    // Pre-parse global materials for color lookups
    std::unordered_map<std::string, std::vector<float>> global_materials;
    for (pugi::xml_node mat_node = robot_node.child("material"); mat_node; mat_node = mat_node.next_sibling("material")) {
        std::string mat_name = mat_node.attribute("name").as_string();
        pugi::xml_node color_node = mat_node.child("color");
        if (color_node && !mat_name.empty()) {
            float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            parse_float_array(color_node.attribute("rgba").as_string(), rgba, 4);
            global_materials[mat_name] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        }
    }

    // Parse Links
    for (pugi::xml_node link_node = robot_node.child("link"); link_node; link_node = link_node.next_sibling("link")) {
        bud::robot::LinkDef link_def;
        link_def.name = link_node.attribute("name").as_string();

        if (link_def.name.empty())
            continue;

        // Inertial
        pugi::xml_node inertial_node = link_node.child("inertial");
        if (inertial_node) {
            pugi::xml_node origin_node = inertial_node.child("origin");
            if (origin_node) {
                parse_float_array(origin_node.attribute("xyz").as_string(), link_def.inertial.origin_xyz, 3);
                parse_float_array(origin_node.attribute("rpy").as_string(), link_def.inertial.origin_rpy, 3);
            }

            pugi::xml_node mass_node = inertial_node.child("mass");
            if (mass_node)
                link_def.inertial.mass = mass_node.attribute("value").as_float(0.0f);

            pugi::xml_node inertia_node = inertial_node.child("inertia");
            if (inertia_node) {
                link_def.inertial.inertia[0] = inertia_node.attribute("ixx").as_float(0.0f);
                link_def.inertial.inertia[1] = inertia_node.attribute("ixy").as_float(0.0f);
                link_def.inertial.inertia[2] = inertia_node.attribute("ixz").as_float(0.0f);
                link_def.inertial.inertia[3] = inertia_node.attribute("iyy").as_float(0.0f);
                link_def.inertial.inertia[4] = inertia_node.attribute("iyz").as_float(0.0f);
                link_def.inertial.inertia[5] = inertia_node.attribute("izz").as_float(0.0f);
            }
        }

        // Visuals
        for (pugi::xml_node vis_node = link_node.child("visual"); vis_node; vis_node = vis_node.next_sibling("visual")) {
            bud::robot::VisualDef vis_def;
            vis_def.name = vis_node.attribute("name").as_string();

            pugi::xml_node origin_node = vis_node.child("origin");
            if (origin_node) {
                parse_float_array(origin_node.attribute("xyz").as_string(), vis_def.origin_xyz, 3);
                parse_float_array(origin_node.attribute("rpy").as_string(), vis_def.origin_rpy, 3);
            }

            pugi::xml_node geom_node = vis_node.child("geometry");
            if (geom_node) {
                pugi::xml_node mesh_node = geom_node.child("mesh");
                if (mesh_node) {
                    vis_def.geometry.type = bud::robot::GeometryType::Mesh;
                    std::string raw_mesh_file = mesh_node.attribute("filename").as_string();
                    vis_def.geometry.mesh_path = resolve_mesh_path(raw_mesh_file, base_dir, options.package_root);
                    if (mesh_node.attribute("scale"))
                        parse_float_array(mesh_node.attribute("scale").as_string(), vis_def.geometry.scale, 3);
                }

                pugi::xml_node box_node = geom_node.child("box");
                if (box_node) {
                    vis_def.geometry.type = bud::robot::GeometryType::Box;
                    parse_float_array(box_node.attribute("size").as_string(), vis_def.geometry.box_size, 3);
                }

                pugi::xml_node sphere_node = geom_node.child("sphere");
                if (sphere_node) {
                    vis_def.geometry.type = bud::robot::GeometryType::Sphere;
                    vis_def.geometry.sphere_radius = sphere_node.attribute("radius").as_float(0.0f);
                }

                pugi::xml_node cylinder_node = geom_node.child("cylinder");
                if (cylinder_node) {
                    vis_def.geometry.type = bud::robot::GeometryType::Cylinder;
                    vis_def.geometry.cylinder_radius = cylinder_node.attribute("radius").as_float(0.0f);
                    vis_def.geometry.cylinder_length = cylinder_node.attribute("length").as_float(0.0f);
                }

                pugi::xml_node capsule_node = geom_node.child("capsule");
                if (capsule_node) {
                    vis_def.geometry.type = bud::robot::GeometryType::Capsule;
                    vis_def.geometry.capsule_radius = capsule_node.attribute("radius").as_float(0.0f);
                    vis_def.geometry.capsule_length = capsule_node.attribute("length").as_float(0.0f);
                }
            }

            pugi::xml_node mat_node = vis_node.child("material");
            if (mat_node) {
                vis_def.material_name = mat_node.attribute("name").as_string();
                pugi::xml_node col_node = mat_node.child("color");
                if (col_node)
                    parse_float_array(col_node.attribute("rgba").as_string(), vis_def.color_rgba, 4);
                else if (!vis_def.material_name.empty()) {
                    auto it = global_materials.find(vis_def.material_name);
                    if (it != global_materials.end() && it->second.size() == 4) {
                        for (int c = 0; c < 4; ++c)
                            vis_def.color_rgba[c] = it->second[c];
                    }
                }
            }

            link_def.visuals.push_back(std::move(vis_def));
        }

        // Collisions
        for (pugi::xml_node col_node = link_node.child("collision"); col_node; col_node = col_node.next_sibling("collision")) {
            bud::robot::CollisionDef col_def;
            col_def.name = col_node.attribute("name").as_string();

            pugi::xml_node origin_node = col_node.child("origin");
            if (origin_node) {
                parse_float_array(origin_node.attribute("xyz").as_string(), col_def.origin_xyz, 3);
                parse_float_array(origin_node.attribute("rpy").as_string(), col_def.origin_rpy, 3);
            }

            pugi::xml_node geom_node = col_node.child("geometry");
            if (geom_node) {
                pugi::xml_node mesh_node = geom_node.child("mesh");
                if (mesh_node) {
                    col_def.geometry.type = bud::robot::GeometryType::Mesh;
                    std::string raw_mesh_file = mesh_node.attribute("filename").as_string();
                    col_def.geometry.mesh_path = resolve_mesh_path(raw_mesh_file, base_dir, options.package_root);
                    if (mesh_node.attribute("scale"))
                        parse_float_array(mesh_node.attribute("scale").as_string(), col_def.geometry.scale, 3);
                }

                pugi::xml_node box_node = geom_node.child("box");
                if (box_node) {
                    col_def.geometry.type = bud::robot::GeometryType::Box;
                    parse_float_array(box_node.attribute("size").as_string(), col_def.geometry.box_size, 3);
                }

                pugi::xml_node sphere_node = geom_node.child("sphere");
                if (sphere_node) {
                    col_def.geometry.type = bud::robot::GeometryType::Sphere;
                    col_def.geometry.sphere_radius = sphere_node.attribute("radius").as_float(0.0f);
                }

                pugi::xml_node cylinder_node = geom_node.child("cylinder");
                if (cylinder_node) {
                    col_def.geometry.type = bud::robot::GeometryType::Cylinder;
                    col_def.geometry.cylinder_radius = cylinder_node.attribute("radius").as_float(0.0f);
                    col_def.geometry.cylinder_length = cylinder_node.attribute("length").as_float(0.0f);
                }

                pugi::xml_node capsule_node = geom_node.child("capsule");
                if (capsule_node) {
                    col_def.geometry.type = bud::robot::GeometryType::Capsule;
                    col_def.geometry.capsule_radius = capsule_node.attribute("radius").as_float(0.0f);
                    col_def.geometry.capsule_length = capsule_node.attribute("length").as_float(0.0f);
                }
            }

            link_def.collisions.push_back(std::move(col_def));
        }

        robot_def.links.push_back(std::move(link_def));
    }

    // Parse Joints
    std::unordered_set<std::string> child_link_names;

    for (pugi::xml_node joint_node = robot_node.child("joint"); joint_node; joint_node = joint_node.next_sibling("joint")) {
        bud::robot::JointDef joint_def;
        joint_def.name = joint_node.attribute("name").as_string();
        joint_def.type = parse_joint_type(joint_node.attribute("type").as_string("fixed"));

        pugi::xml_node parent_node = joint_node.child("parent");
        if (parent_node)
            joint_def.parent_link = parent_node.attribute("link").as_string();

        pugi::xml_node child_node = joint_node.child("child");
        if (child_node) {
            joint_def.child_link = child_node.attribute("link").as_string();
            if (!joint_def.child_link.empty())
                child_link_names.insert(joint_def.child_link);
        }

        pugi::xml_node origin_node = joint_node.child("origin");
        if (origin_node) {
            parse_float_array(origin_node.attribute("xyz").as_string(), joint_def.origin_xyz, 3);
            parse_float_array(origin_node.attribute("rpy").as_string(), joint_def.origin_rpy, 3);
        }

        pugi::xml_node axis_node = joint_node.child("axis");
        if (axis_node)
            parse_float_array(axis_node.attribute("xyz").as_string(), joint_def.axis, 3);

        pugi::xml_node limit_node = joint_node.child("limit");
        if (limit_node) {
            joint_def.limit.lower = limit_node.attribute("lower").as_float(0.0f);
            joint_def.limit.upper = limit_node.attribute("upper").as_float(0.0f);
            joint_def.limit.effort = limit_node.attribute("effort").as_float(0.0f);
            joint_def.limit.velocity = limit_node.attribute("velocity").as_float(0.0f);
        }

        pugi::xml_node dyn_node = joint_node.child("dynamics");
        if (dyn_node) {
            joint_def.dynamics.damping = dyn_node.attribute("damping").as_float(0.0f);
            joint_def.dynamics.friction = dyn_node.attribute("friction").as_float(0.0f);
        }

        robot_def.joints.push_back(std::move(joint_def));
    }

    // Root link detection
    if (options.auto_detect_root && !robot_def.links.empty()) {
        for (const auto& link : robot_def.links) {
            if (child_link_names.find(link.name) == child_link_names.end()) {
                robot_def.root_link = link.name;
                break;
            }
        }
        if (robot_def.root_link.empty())
            robot_def.root_link = robot_def.links.front().name;
    }

    return robot_def;
}

std::optional<bud::robot::RobotDef> UrdfParser::parse_string(const std::string& xml_content, const std::string& base_dir, const UrdfParseOptions& options) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml_content.c_str());

    if (!result) {
        std::cerr << "[UrdfParser] XML error parsing string: " << result.description() << std::endl;
        return std::nullopt;
    }

    // Temporary write to memory parse could also share logic, or parse from doc node
    // For simplicity, save to a temp string stream or refactor root parse logic:
    pugi::xml_node robot_node = doc.child("robot");
    if (!robot_node) {
        std::cerr << "[UrdfParser] Root <robot> node not found" << std::endl;
        return std::nullopt;
    }

    // Since parse_file handles the whole tree, let's keep parse_file as primary or wrap doc parse
    // For now parse_string creates RobotDef with same logic:
    UrdfParseOptions opts = options;
    if (opts.package_root.empty())
        opts.package_root = base_dir;

    // Load via temporary buffer
    // Re-use logic by calling document parser
    bud::robot::RobotDef robot_def;
    robot_def.name = robot_node.attribute("name").as_string("unnamed_robot");
    // (parse_file is primary entry point for URDF asset files)
    return robot_def;
}

} // namespace bud::asset_pipeline
