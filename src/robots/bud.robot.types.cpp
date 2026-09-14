#include "bud.robot.types.hpp"
#include <fstream>
#include <cstring>
#include <nlohmann/json.hpp>

namespace bud::robots {

const LinkDef* RobotDef::find_link(const std::string& link_name) const {
    for (const auto& link : links) {
        if (link.name == link_name)
            return &link;
    }
    return nullptr;
}

LinkDef* RobotDef::find_link(const std::string& link_name) {
    for (auto& link : links) {
        if (link.name == link_name)
            return &link;
    }
    return nullptr;
}

const JointDef* RobotDef::find_joint(const std::string& joint_name) const {
    for (const auto& joint : joints) {
        if (joint.name == joint_name)
            return &joint;
    }
    return nullptr;
}

JointDef* RobotDef::find_joint(const std::string& joint_name) {
    for (auto& joint : joints) {
        if (joint.name == joint_name)
            return &joint;
    }
    return nullptr;
}

std::vector<const JointDef*> RobotDef::get_child_joints(const std::string& link_name) const {
    std::vector<const JointDef*> result;
    for (const auto& joint : joints) {
        if (joint.parent_link == link_name)
            result.push_back(&joint);
    }
    return result;
}

const JointDef* RobotDef::get_parent_joint(const std::string& link_name) const {
    for (const auto& joint : joints) {
        if (joint.child_link == link_name)
            return &joint;
    }
    return nullptr;
}

std::vector<uint8_t> RobotDef::serialize_binary() const {
    std::vector<uint8_t> buffer;

    auto write_bytes = [&](const void* ptr, size_t sz) {
        const uint8_t* byte_ptr = reinterpret_cast<const uint8_t*>(ptr);
        buffer.insert(buffer.end(), byte_ptr, byte_ptr + sz);
    };

    auto write_str = [&](const std::string& str) {
        uint32_t len = static_cast<uint32_t>(str.length());
        write_bytes(&len, sizeof(len));
        if (len > 0)
            write_bytes(str.data(), len);
    };

    uint64_t magic = ARTICULATION_CHUNK_MAGIC;
    uint32_t version = ARTICULATION_CHUNK_VERSION;
    write_bytes(&magic, sizeof(magic));
    write_bytes(&version, sizeof(version));

    write_str(name);
    write_str(root_link);

    // Links
    uint32_t link_count = static_cast<uint32_t>(links.size());
    write_bytes(&link_count, sizeof(link_count));
    for (const auto& l : links) {
        write_str(l.name);
        write_bytes(&l.inertial, sizeof(InertialDef));

        // Visuals
        uint32_t vis_count = static_cast<uint32_t>(l.visuals.size());
        write_bytes(&vis_count, sizeof(vis_count));
        for (const auto& v : l.visuals) {
            write_str(v.name);
            write_bytes(v.origin_xyz, sizeof(float) * 3);
            write_bytes(v.origin_rpy, sizeof(float) * 3);
            uint8_t geom_type = static_cast<uint8_t>(v.geometry.type);
            write_bytes(&geom_type, sizeof(geom_type));
            write_str(v.geometry.mesh_path);
            write_bytes(v.geometry.scale, sizeof(float) * 3);
            write_bytes(v.geometry.box_size, sizeof(float) * 3);
            write_bytes(&v.geometry.sphere_radius, sizeof(float));
            write_bytes(&v.geometry.cylinder_radius, sizeof(float));
            write_bytes(&v.geometry.cylinder_length, sizeof(float));
            write_bytes(&v.geometry.capsule_radius, sizeof(float));
            write_bytes(&v.geometry.capsule_length, sizeof(float));
            write_bytes(v.color_rgba, sizeof(float) * 4);
            write_str(v.material_name);
        }

        // Collisions
        uint32_t col_count = static_cast<uint32_t>(l.collisions.size());
        write_bytes(&col_count, sizeof(col_count));
        for (const auto& c : l.collisions) {
            write_str(c.name);
            write_bytes(c.origin_xyz, sizeof(float) * 3);
            write_bytes(c.origin_rpy, sizeof(float) * 3);
            uint8_t geom_type = static_cast<uint8_t>(c.geometry.type);
            write_bytes(&geom_type, sizeof(geom_type));
            write_str(c.geometry.mesh_path);
            write_bytes(c.geometry.scale, sizeof(float) * 3);
            write_bytes(c.geometry.box_size, sizeof(float) * 3);
            write_bytes(&c.geometry.sphere_radius, sizeof(float));
            write_bytes(&c.geometry.cylinder_radius, sizeof(float));
            write_bytes(&c.geometry.cylinder_length, sizeof(float));
            write_bytes(&c.geometry.capsule_radius, sizeof(float));
            write_bytes(&c.geometry.capsule_length, sizeof(float));

            // Convex Hull
            uint32_t pt_count = static_cast<uint32_t>(c.convex_hull.points.size());
            write_bytes(&pt_count, sizeof(pt_count));
            if (pt_count > 0)
                write_bytes(c.convex_hull.points.data(), pt_count * sizeof(float));

            uint32_t idx_count = static_cast<uint32_t>(c.convex_hull.indices.size());
            write_bytes(&idx_count, sizeof(idx_count));
            if (idx_count > 0)
                write_bytes(c.convex_hull.indices.data(), idx_count * sizeof(uint32_t));

            write_bytes(c.convex_hull.aabb_min, sizeof(float) * 3);
            write_bytes(c.convex_hull.aabb_max, sizeof(float) * 3);
            write_bytes(c.convex_hull.center_of_mass, sizeof(float) * 3);
            write_bytes(&c.convex_hull.volume, sizeof(float));
        }
    }

    // Joints
    uint32_t joint_count = static_cast<uint32_t>(joints.size());
    write_bytes(&joint_count, sizeof(joint_count));
    for (const auto& j : joints) {
        write_str(j.name);
        uint8_t j_type = static_cast<uint8_t>(j.type);
        write_bytes(&j_type, sizeof(j_type));
        write_str(j.parent_link);
        write_str(j.child_link);
        write_bytes(j.origin_xyz, sizeof(float) * 3);
        write_bytes(j.origin_rpy, sizeof(float) * 3);
        write_bytes(j.axis, sizeof(float) * 3);
        write_bytes(&j.limit, sizeof(JointLimit));
        write_bytes(&j.dynamics, sizeof(JointDynamics));
        write_bytes(&j.motor, sizeof(JointMotor));
    }

    return buffer;
}

std::optional<RobotDef> RobotDef::deserialize_binary(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(uint64_t) + sizeof(uint32_t))
        return std::nullopt;

    size_t offset = 0;
    auto read_bytes = [&](void* dst, size_t sz) -> bool {
        if (offset + sz > size)
            return false;
        std::memcpy(dst, data + offset, sz);
        offset += sz;
        return true;
    };

    auto read_str = [&](std::string& out_str) -> bool {
        uint32_t len = 0;
        if (!read_bytes(&len, sizeof(len)))
            return false;
        if (len > 0) {
            if (offset + len > size)
                return false;
            out_str.assign(reinterpret_cast<const char*>(data + offset), len);
            offset += len;
        } else {
            out_str.clear();
        }
        return true;
    };

    uint64_t magic = 0;
    uint32_t version = 0;
    if (!read_bytes(&magic, sizeof(magic)) || !read_bytes(&version, sizeof(version)))
        return std::nullopt;

    if (magic != ARTICULATION_CHUNK_MAGIC || version != ARTICULATION_CHUNK_VERSION)
        return std::nullopt;

    RobotDef robot;
    if (!read_str(robot.name) || !read_str(robot.root_link))
        return std::nullopt;

    // Links
    uint32_t link_count = 0;
    if (!read_bytes(&link_count, sizeof(link_count)))
        return std::nullopt;
    robot.links.resize(link_count);

    for (uint32_t i = 0; i < link_count; ++i) {
        auto& l = robot.links[i];
        if (!read_str(l.name) || !read_bytes(&l.inertial, sizeof(InertialDef)))
            return std::nullopt;

        // Visuals
        uint32_t vis_count = 0;
        if (!read_bytes(&vis_count, sizeof(vis_count)))
            return std::nullopt;
        l.visuals.resize(vis_count);
        for (uint32_t vi = 0; vi < vis_count; ++vi) {
            auto& v = l.visuals[vi];
            if (!read_str(v.name) ||
                !read_bytes(v.origin_xyz, sizeof(float) * 3) ||
                !read_bytes(v.origin_rpy, sizeof(float) * 3))
                return std::nullopt;

            uint8_t geom_type = 0;
            if (!read_bytes(&geom_type, sizeof(geom_type)))
                return std::nullopt;
            v.geometry.type = static_cast<GeometryType>(geom_type);

            if (!read_str(v.geometry.mesh_path) ||
                !read_bytes(v.geometry.scale, sizeof(float) * 3) ||
                !read_bytes(v.geometry.box_size, sizeof(float) * 3) ||
                !read_bytes(&v.geometry.sphere_radius, sizeof(float)) ||
                !read_bytes(&v.geometry.cylinder_radius, sizeof(float)) ||
                !read_bytes(&v.geometry.cylinder_length, sizeof(float)) ||
                !read_bytes(&v.geometry.capsule_radius, sizeof(float)) ||
                !read_bytes(&v.geometry.capsule_length, sizeof(float)) ||
                !read_bytes(v.color_rgba, sizeof(float) * 4) ||
                !read_str(v.material_name))
                return std::nullopt;
        }

        // Collisions
        uint32_t col_count = 0;
        if (!read_bytes(&col_count, sizeof(col_count)))
            return std::nullopt;
        l.collisions.resize(col_count);
        for (uint32_t ci = 0; ci < col_count; ++ci) {
            auto& c = l.collisions[ci];
            if (!read_str(c.name) ||
                !read_bytes(c.origin_xyz, sizeof(float) * 3) ||
                !read_bytes(c.origin_rpy, sizeof(float) * 3))
                return std::nullopt;

            uint8_t geom_type = 0;
            if (!read_bytes(&geom_type, sizeof(geom_type)))
                return std::nullopt;
            c.geometry.type = static_cast<GeometryType>(geom_type);

            if (!read_str(c.geometry.mesh_path) ||
                !read_bytes(c.geometry.scale, sizeof(float) * 3) ||
                !read_bytes(c.geometry.box_size, sizeof(float) * 3) ||
                !read_bytes(&c.geometry.sphere_radius, sizeof(float)) ||
                !read_bytes(&c.geometry.cylinder_radius, sizeof(float)) ||
                !read_bytes(&c.geometry.cylinder_length, sizeof(float)) ||
                !read_bytes(&c.geometry.capsule_radius, sizeof(float)) ||
                !read_bytes(&c.geometry.capsule_length, sizeof(float)))
                return std::nullopt;

            uint32_t pt_count = 0;
            if (!read_bytes(&pt_count, sizeof(pt_count)))
                return std::nullopt;
            if (pt_count > 0) {
                size_t pt_bytes = pt_count * sizeof(float);
                if (offset + pt_bytes > size)
                    return std::nullopt;
                c.convex_hull.points.resize(pt_count);
                std::memcpy(c.convex_hull.points.data(), data + offset, pt_bytes);
                offset += pt_bytes;
            }

            uint32_t idx_count = 0;
            if (!read_bytes(&idx_count, sizeof(idx_count)))
                return std::nullopt;
            if (idx_count > 0) {
                size_t idx_bytes = idx_count * sizeof(uint32_t);
                if (offset + idx_bytes > size)
                    return std::nullopt;
                c.convex_hull.indices.resize(idx_count);
                std::memcpy(c.convex_hull.indices.data(), data + offset, idx_bytes);
                offset += idx_bytes;
            }

            if (!read_bytes(c.convex_hull.aabb_min, sizeof(float) * 3) ||
                !read_bytes(c.convex_hull.aabb_max, sizeof(float) * 3) ||
                !read_bytes(c.convex_hull.center_of_mass, sizeof(float) * 3) ||
                !read_bytes(&c.convex_hull.volume, sizeof(float)))
                return std::nullopt;
        }
    }

    // Joints
    uint32_t joint_count = 0;
    if (!read_bytes(&joint_count, sizeof(joint_count)))
        return std::nullopt;
    robot.joints.resize(joint_count);

    for (uint32_t ji = 0; ji < joint_count; ++ji) {
        auto& j = robot.joints[ji];
        uint8_t j_type = 0;
        if (!read_str(j.name) ||
            !read_bytes(&j_type, sizeof(j_type)) ||
            !read_str(j.parent_link) ||
            !read_str(j.child_link) ||
            !read_bytes(j.origin_xyz, sizeof(float) * 3) ||
            !read_bytes(j.origin_rpy, sizeof(float) * 3) ||
            !read_bytes(j.axis, sizeof(float) * 3) ||
            !read_bytes(&j.limit, sizeof(JointLimit)) ||
            !read_bytes(&j.dynamics, sizeof(JointDynamics)) ||
            !read_bytes(&j.motor, sizeof(JointMotor)))
            return std::nullopt;

        j.type = static_cast<JointType>(j_type);
    }

    return robot;
}

bool RobotDef::save_json(const std::string& path) const {
    nlohmann::json j;
    j["name"] = name;
    j["root_link"] = root_link;

    auto& j_links = j["links"];
    for (const auto& l : links) {
        nlohmann::json jl;
        jl["name"] = l.name;
        jl["inertial"] = {
            { "origin_xyz", { l.inertial.origin_xyz[0], l.inertial.origin_xyz[1], l.inertial.origin_xyz[2] } },
            { "origin_rpy", { l.inertial.origin_rpy[0], l.inertial.origin_rpy[1], l.inertial.origin_rpy[2] } },
            { "mass", l.inertial.mass },
            { "inertia", { l.inertial.inertia[0], l.inertial.inertia[1], l.inertial.inertia[2],
                           l.inertial.inertia[3], l.inertial.inertia[4], l.inertial.inertia[5] } }
        };

        auto& j_vis = jl["visuals"];
        for (const auto& v : l.visuals) {
            jl["visuals"].push_back({
                { "name", v.name },
                { "origin_xyz", { v.origin_xyz[0], v.origin_xyz[1], v.origin_xyz[2] } },
                { "origin_rpy", { v.origin_rpy[0], v.origin_rpy[1], v.origin_rpy[2] } },
                { "geometry", {
                    { "type", static_cast<uint8_t>(v.geometry.type) },
                    { "mesh_path", v.geometry.mesh_path },
                    { "scale", { v.geometry.scale[0], v.geometry.scale[1], v.geometry.scale[2] } }
                } },
                { "color_rgba", { v.color_rgba[0], v.color_rgba[1], v.color_rgba[2], v.color_rgba[3] } },
                { "material_name", v.material_name }
            });
        }

        auto& j_col = jl["collisions"];
        for (const auto& c : l.collisions) {
            jl["collisions"].push_back({
                { "name", c.name },
                { "origin_xyz", { c.origin_xyz[0], c.origin_xyz[1], c.origin_xyz[2] } },
                { "origin_rpy", { c.origin_rpy[0], c.origin_rpy[1], c.origin_rpy[2] } },
                { "geometry", {
                    { "type", static_cast<uint8_t>(c.geometry.type) },
                    { "mesh_path", c.geometry.mesh_path },
                    { "scale", { c.geometry.scale[0], c.geometry.scale[1], c.geometry.scale[2] } }
                } }
            });
        }

        j_links.push_back(jl);
    }

    auto& j_joints = j["joints"];
    for (const auto& joint : joints) {
        j_joints.push_back({
            { "name", joint.name },
            { "type", static_cast<uint8_t>(joint.type) },
            { "parent_link", joint.parent_link },
            { "child_link", joint.child_link },
            { "origin_xyz", { joint.origin_xyz[0], joint.origin_xyz[1], joint.origin_xyz[2] } },
            { "origin_rpy", { joint.origin_rpy[0], joint.origin_rpy[1], joint.origin_rpy[2] } },
            { "axis", { joint.axis[0], joint.axis[1], joint.axis[2] } },
            { "limit", {
                { "lower", joint.limit.lower },
                { "upper", joint.limit.upper },
                { "effort", joint.limit.effort },
                { "velocity", joint.limit.velocity }
            } },
            { "dynamics", {
                { "damping", joint.dynamics.damping },
                { "friction", joint.dynamics.friction }
            } },
            { "motor", {
                { "enabled", joint.motor.enabled },
                { "stiffness", joint.motor.stiffness },
                { "damping", joint.motor.damping },
                { "max_force", joint.motor.max_force }
            } }
        });
    }

    std::ofstream file(path);
    if (!file.is_open())
        return false;

    file << j.dump(2);
    return file.good();
}

std::optional<RobotDef> RobotDef::load_from_budasset(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open())
        return std::nullopt;

    size_t sz = static_cast<size_t>(in.tellg());
    in.seekg(0);
    if (sz < sizeof(bud::asset::BudAssetHeader))
        return std::nullopt;

    std::vector<uint8_t> buf(sz);
    in.read(reinterpret_cast<char*>(buf.data()), sz);

    const auto* header = reinterpret_cast<const bud::asset::BudAssetHeader*>(buf.data());
    if (header->magic != bud::asset::BUD_ASSET_MAGIC)
        return std::nullopt;

    if (header->chunk_table_offset + header->chunk_count * sizeof(bud::asset::AssetChunkEntry) > sz)
        return std::nullopt;

    const auto* chunks = reinterpret_cast<const bud::asset::AssetChunkEntry*>(buf.data() + header->chunk_table_offset);
    for (uint32_t i = 0; i < header->chunk_count; ++i) {
        if (chunks[i].chunk_type == static_cast<uint32_t>(bud::asset::AssetChunkType::Articulation)) {
            if (chunks[i].offset + chunks[i].size <= sz)
                return deserialize_binary(buf.data() + chunks[i].offset, chunks[i].size);
        }
    }
    return std::nullopt;
}

} // namespace bud::robots
