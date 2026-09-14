#include "src/robots/bud.robot.loader.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>

#include <iostream>
#include <queue>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <Jolt/Geometry/ConvexHullBuilder.h>
#include "src/core/bud.asset.types.hpp"
#include "src/core/bud.raw_mesh.hpp"

namespace bud::robots {

namespace {

std::optional<bud::asset::RawMesh> load_raw_mesh_from_budasset_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return std::nullopt;

    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    if (file_size < sizeof(bud::asset::BudAssetHeader))
        return std::nullopt;

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);

    const auto* header = reinterpret_cast<const bud::asset::BudAssetHeader*>(buffer.data());
    if (header->magic != bud::asset::BUD_ASSET_MAGIC)
        return std::nullopt;

    if (header->chunk_table_offset + header->chunk_count * sizeof(bud::asset::AssetChunkEntry) > file_size)
        return std::nullopt;

    const auto* chunks = reinterpret_cast<const bud::asset::AssetChunkEntry*>(buffer.data() + header->chunk_table_offset);
    for (uint32_t c = 0; c < header->chunk_count; ++c) {
        if (chunks[c].chunk_type == static_cast<uint32_t>(bud::asset::AssetChunkType::RawMesh)) {
            if (chunks[c].offset + chunks[c].size <= file_size)
                return bud::asset::RawMesh::deserialize_binary(buffer.data() + chunks[c].offset, chunks[c].size);
        }
    }
    return std::nullopt;
}

std::optional<bud::robots::ConvexHullData> build_convex_hull_from_raw_mesh(const bud::asset::RawMesh& mesh, int max_vertices) {
    if (mesh.vertices.empty())
        return std::nullopt;

    JPH::Array<JPH::Vec3> in_positions;
    in_positions.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) {
        in_positions.push_back(JPH::Vec3(v.position[0], v.position[1], v.position[2]));
    }

    bud::robots::ConvexHullData hull_data;
    hull_data.aabb_min[0] = hull_data.aabb_min[1] = hull_data.aabb_min[2] = 1e30f;
    hull_data.aabb_max[0] = hull_data.aabb_max[1] = hull_data.aabb_max[2] = -1e30f;

    const char* error_msg = nullptr;
    constexpr float hull_tolerance = 1.0e-3f;
    JPH::ConvexHullBuilder builder(in_positions);
    JPH::ConvexHullBuilder::EResult result = builder.Initialize(max_vertices, hull_tolerance, error_msg);

    if (result == JPH::ConvexHullBuilder::EResult::Success ||
        result == JPH::ConvexHullBuilder::EResult::MaxVerticesReached) {

        JPH::Vec3 com{ 0.0f, 0.0f, 0.0f };
        float volume = 0.0f;
        builder.GetCenterOfMassAndVolume(com, volume);

        hull_data.center_of_mass[0] = com.GetX();
        hull_data.center_of_mass[1] = com.GetY();
        hull_data.center_of_mass[2] = com.GetZ();
        hull_data.volume = volume;

        std::unordered_map<int, uint32_t> global_to_local;
        const auto& faces = builder.GetFaces();

        for (const auto* face : faces) {
            if (!face || face->mRemoved)
                continue;

            std::vector<int> face_indices;
            const JPH::ConvexHullBuilder::Edge* first_edge = face->mFirstEdge;
            const JPH::ConvexHullBuilder::Edge* edge = first_edge;

            while (edge) {
                face_indices.push_back(edge->mStartIdx);
                edge = edge->mNextEdge;
                if (edge == first_edge)
                    break;
            }

            if (face_indices.size() < 3)
                continue;

            std::vector<uint32_t> local_poly;
            local_poly.reserve(face_indices.size());
            for (int orig_idx : face_indices) {
                auto it = global_to_local.find(orig_idx);
                if (it == global_to_local.end()) {
                    uint32_t new_idx = static_cast<uint32_t>(hull_data.points.size() / 3);
                    const JPH::Vec3& p = in_positions[orig_idx];
                    hull_data.points.push_back(p.GetX());
                    hull_data.points.push_back(p.GetY());
                    hull_data.points.push_back(p.GetZ());

                    hull_data.aabb_min[0] = std::min(hull_data.aabb_min[0], p.GetX());
                    hull_data.aabb_min[1] = std::min(hull_data.aabb_min[1], p.GetY());
                    hull_data.aabb_min[2] = std::min(hull_data.aabb_min[2], p.GetZ());

                    hull_data.aabb_max[0] = std::max(hull_data.aabb_max[0], p.GetX());
                    hull_data.aabb_max[1] = std::max(hull_data.aabb_max[1], p.GetY());
                    hull_data.aabb_max[2] = std::max(hull_data.aabb_max[2], p.GetZ());

                    global_to_local[orig_idx] = new_idx;
                    local_poly.push_back(new_idx);
                }
                else {
                    local_poly.push_back(it->second);
                }
            }

            for (size_t i = 1; i + 1 < local_poly.size(); ++i) {
                hull_data.indices.push_back(local_poly[0]);
                hull_data.indices.push_back(local_poly[i]);
                hull_data.indices.push_back(local_poly[i + 1]);
            }
        }
        return hull_data;
    }

    return std::nullopt;
}

void ensure_link_mesh_collisions(bud::robots::RobotDef& robot_def, const std::string& package_root) {
    std::filesystem::path root_dir(package_root);
    for (auto& link : robot_def.links) {
        const bool is_hand_or_foot = (link.name.find("hand") != std::string::npos ||
                                      link.name.find("ankle_roll") != std::string::npos ||
                                      link.name.find("foot") != std::string::npos);

        bool has_mesh_col = false;
        for (const auto& col : link.collisions) {
            if (col.geometry.type == bud::robots::GeometryType::Mesh && !col.convex_hull.points.empty()) {
                has_mesh_col = true;
                break;
            }
        }

        if (is_hand_or_foot && !has_mesh_col && !link.visuals.empty()) {
            const auto& vis = link.visuals.front();
            if (!vis.geometry.mesh_path.empty()) {
                std::filesystem::path mesh_file = root_dir / vis.geometry.mesh_path;
                auto raw_mesh_opt = load_raw_mesh_from_budasset_file(mesh_file.string());
                if (raw_mesh_opt) {
                    auto hull_opt = build_convex_hull_from_raw_mesh(*raw_mesh_opt, 64);
                    if (hull_opt) {
                        bud::robots::CollisionDef col_def;
                        col_def.name = link.name + "_collision";
                        for (int i = 0; i < 3; ++i) {
                            col_def.origin_xyz[i] = vis.origin_xyz[i];
                            col_def.origin_rpy[i] = vis.origin_rpy[i];
                            col_def.geometry.scale[i] = vis.geometry.scale[i];
                        }
                        col_def.geometry.type = bud::robots::GeometryType::Mesh;
                        col_def.geometry.mesh_path = vis.geometry.mesh_path;
                        col_def.convex_hull = std::move(*hull_opt);

                        link.collisions.clear();
                        link.collisions.push_back(std::move(col_def));
                        const auto& stored_col = link.collisions.back();
                        std::cout << "[RobotLoader] Synthesized convex hull collision for link '"
                                  << link.name << "': " << stored_col.convex_hull.points.size() / 3
                                  << " vertices, " << stored_col.convex_hull.indices.size() / 3
                                  << " triangles." << std::endl;
                    }
                }
            }
        }
    }
}


JPH::Quat rpy_to_quat(float roll, float pitch, float yaw) {
    // URDF convention: extrinsic Rz(yaw) * Ry(pitch) * Rx(roll)
    return (JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), yaw)
          * JPH::Quat::sRotation(JPH::Vec3::sAxisY(), pitch)
          * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), roll)).Normalized();
}

struct NodeTransform {
    JPH::RVec3 position{ 0.0f, 0.0f, 0.0f };
    JPH::Quat rotation = JPH::Quat::sIdentity();
};

constexpr float k_min_shape_dimension = 0.005f;
constexpr float k_default_proxy_size = 0.02f;

JPH::Ref<JPH::ShapeSettings> create_link_shape(const bud::robots::LinkDef& link) {
    for (const auto& col : link.collisions) {
        JPH::Ref<JPH::ShapeSettings> base_shape = nullptr;

        if (col.geometry.type == bud::robots::GeometryType::Mesh && !col.convex_hull.points.empty()) {
            JPH::Array<JPH::Vec3> points;
            size_t pt_count = col.convex_hull.points.size() / 3;
            points.reserve(pt_count);
            for (size_t i = 0; i < pt_count; ++i) {
                points.push_back(JPH::Vec3(
                    col.convex_hull.points[i * 3 + 0],
                    col.convex_hull.points[i * 3 + 1],
                    col.convex_hull.points[i * 3 + 2]
                ));
            }
            base_shape = new JPH::ConvexHullShapeSettings(points);
        }
        else if (col.geometry.type == bud::robots::GeometryType::Box) {
            float hx = std::max(k_min_shape_dimension, col.geometry.box_size[0] * 0.5f);
            float hy = std::max(k_min_shape_dimension, col.geometry.box_size[1] * 0.5f);
            float hz = std::max(k_min_shape_dimension, col.geometry.box_size[2] * 0.5f);
            base_shape = new JPH::BoxShapeSettings(JPH::Vec3(hx, hy, hz));
        }
        else if (col.geometry.type == bud::robots::GeometryType::Sphere) {
            float r = std::max(k_min_shape_dimension, col.geometry.sphere_radius);
            base_shape = new JPH::SphereShapeSettings(r);
        }
        else if (col.geometry.type == bud::robots::GeometryType::Cylinder) {
            float r = std::max(k_min_shape_dimension, col.geometry.cylinder_radius);
            float hh = std::max(k_min_shape_dimension, col.geometry.cylinder_length * 0.5f);
            base_shape = new JPH::CylinderShapeSettings(hh, r);
        }
        else if (col.geometry.type == bud::robots::GeometryType::Capsule) {
            float r = std::max(k_min_shape_dimension, col.geometry.capsule_radius);
            float hh = std::max(k_min_shape_dimension, col.geometry.capsule_length * 0.5f);
            base_shape = new JPH::CapsuleShapeSettings(hh, r);
        }

        if (base_shape) {
            JPH::Vec3 col_pos(col.origin_xyz[0], col.origin_xyz[1], col.origin_xyz[2]);
            JPH::Quat col_rot = rpy_to_quat(col.origin_rpy[0], col.origin_rpy[1], col.origin_rpy[2]);
            if (col_pos.LengthSq() > 1e-6f || col_rot != JPH::Quat::sIdentity())
                return new JPH::RotatedTranslatedShapeSettings(col_pos, col_rot, base_shape);
            return base_shape;
        }
    }

    // Default fallback proxy shape if link has no collision geometry
    return new JPH::BoxShapeSettings(JPH::Vec3(k_default_proxy_size, k_default_proxy_size, k_default_proxy_size));
}

} // namespace

RobotInstance::RobotInstance(const bud::robots::RobotDef& def,
                             JPH::Ragdoll* ragdoll,
                             JPH::PhysicsSystem* system,
                             std::unordered_map<std::string, int> link_to_part,
                             std::unordered_map<std::string, int> joint_to_constraint)
    : m_def(def),
      m_ragdoll(ragdoll),
      m_system(system),
      m_link_to_part(std::move(link_to_part)),
      m_joint_to_constraint(std::move(joint_to_constraint)) {
    if (m_ragdoll) {
        m_ragdoll->AddRef();
        for (size_t c_idx = 0; c_idx < m_ragdoll->GetConstraintCount(); ++c_idx) {
            auto* constraint = m_ragdoll->GetConstraint(c_idx);
            if (constraint && constraint->GetSubType() == JPH::EConstraintSubType::Hinge) {
                auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
                hinge->SetMotorState(JPH::EMotorState::Position);
                hinge->SetTargetAngle(0.0f);
            }
        }
    }
}

RobotInstance::~RobotInstance() {
    remove_from_simulation();
}

void RobotInstance::remove_from_simulation() {
    if (m_ragdoll && m_system) {
        m_ragdoll->RemoveFromPhysicsSystem();
        m_ragdoll->Release();
        m_ragdoll = nullptr;
    }
}

void RobotInstance::set_joint_target_angle(const std::string& joint_name, float target_angle_rad) {
    auto it = m_joint_to_constraint.find(joint_name);
    if (it == m_joint_to_constraint.end() || !m_ragdoll)
        return;

    int c_idx = it->second;
    if (c_idx < 0 || static_cast<size_t>(c_idx) >= m_ragdoll->GetConstraintCount())
        return;

    auto* constraint = m_ragdoll->GetConstraint(c_idx);
    if (!constraint)
        return;

    if (constraint->GetSubType() == JPH::EConstraintSubType::Hinge) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
        hinge->SetMotorState(JPH::EMotorState::Position);
        hinge->SetTargetAngle(target_angle_rad);
    }
}

void RobotInstance::set_joint_target_velocity(const std::string& joint_name, float target_velocity_rad_s) {
    auto it = m_joint_to_constraint.find(joint_name);
    if (it == m_joint_to_constraint.end() || !m_ragdoll)
        return;

    int c_idx = it->second;
    if (c_idx < 0 || static_cast<size_t>(c_idx) >= m_ragdoll->GetConstraintCount())
        return;

    auto* constraint = m_ragdoll->GetConstraint(c_idx);
    if (!constraint)
        return;

    if (constraint->GetSubType() == JPH::EConstraintSubType::Hinge) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
        hinge->SetMotorState(JPH::EMotorState::Velocity);
        hinge->SetTargetAngularVelocity(target_velocity_rad_s);
    }
}

float RobotInstance::get_joint_angle(const std::string& joint_name) const {
    auto it = m_joint_to_constraint.find(joint_name);
    if (it == m_joint_to_constraint.end() || !m_ragdoll)
        return 0.0f;

    int c_idx = it->second;
    if (c_idx < 0 || static_cast<size_t>(c_idx) >= m_ragdoll->GetConstraintCount())
        return 0.0f;

    const auto* constraint = m_ragdoll->GetConstraint(c_idx);
    if (!constraint)
        return 0.0f;

    if (constraint->GetSubType() == JPH::EConstraintSubType::Hinge) {
        const auto* hinge = static_cast<const JPH::HingeConstraint*>(constraint);
        return hinge->GetCurrentAngle();
    }

    return 0.0f;
}

bud::math::vec3 RobotInstance::get_link_position(const std::string& link_name) const {
    auto it = m_link_to_part.find(link_name);
    if (it == m_link_to_part.end() || !m_ragdoll || !m_system)
        return {};

    JPH::BodyID bid = m_ragdoll->GetBodyID(it->second);
    JPH::RVec3 pos = m_system->GetBodyInterface().GetPosition(bid);
    return { static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()) };
}

bud::math::quaternion RobotInstance::get_link_rotation(const std::string& link_name) const {
    auto it = m_link_to_part.find(link_name);
    if (it == m_link_to_part.end() || !m_ragdoll || !m_system)
        return bud::math::quaternion(1.0f, 0.0f, 0.0f, 0.0f);

    JPH::BodyID bid = m_ragdoll->GetBodyID(it->second);
    JPH::Quat rot = m_system->GetBodyInterface().GetRotation(bid);
    return bud::math::quaternion(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ());
}

std::vector<RobotInstance::LinkTransform> RobotInstance::get_all_link_transforms() const {
    if (!m_ragdoll || !m_system)
        return {};

    std::vector<LinkTransform> transforms;
    transforms.reserve(m_link_to_part.size());

    const auto& bi = m_system->GetBodyInterfaceNoLock();
    for (const auto& [name, part_idx] : m_link_to_part) {
        if (part_idx < 0 || static_cast<size_t>(part_idx) >= m_ragdoll->GetBodyCount())
            continue;

        JPH::BodyID bid = m_ragdoll->GetBodyID(part_idx);
        JPH::RVec3 pos = bi.GetPosition(bid);
        JPH::Quat rot = bi.GetRotation(bid);
        transforms.push_back({
            name,
            bud::math::vec3(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ())),
            bud::math::quaternion(rot.GetW(), rot.GetX(), rot.GetY(), rot.GetZ())
        });
    }

    return transforms;
}

std::unique_ptr<RobotInstance> RobotLoader::spawn_robot(physics::PhysicsScene& scene,
                                                        const bud::robots::RobotDef& robot_def,
                                                        const RobotSpawnParams& params) {
    JPH::PhysicsSystem* system = scene.get_jolt_system();
    if (!system) {
        std::cerr << "[RobotLoader] Error: Jolt PhysicsSystem not available from PhysicsScene." << std::endl;
        return nullptr;
    }

    if (robot_def.links.empty()) {
        std::cerr << "[RobotLoader] Error: Robot definition has no links." << std::endl;
        return nullptr;
    }

    std::string root_link_name = robot_def.root_link;
    if (root_link_name.empty())
        root_link_name = robot_def.links.front().name;

    // 1. Build BFS Topological Ordering of Links from root
    std::vector<std::string> ordered_link_names;
    std::unordered_map<std::string, int> link_to_skeleton_idx;
    std::unordered_map<std::string, const bud::robots::JointDef*> link_to_parent_joint;
    std::unordered_map<std::string, NodeTransform> link_world_transforms;

    JPH::RVec3 spawn_pos(params.position.x, params.position.y, params.position.z);
    JPH::Quat spawn_rot(params.rotation.x, params.rotation.y, params.rotation.z, params.rotation.w);

    link_world_transforms[root_link_name] = NodeTransform{ spawn_pos, spawn_rot };

    std::queue<std::string> q;
    q.push(root_link_name);
    ordered_link_names.push_back(root_link_name);
    link_to_skeleton_idx[root_link_name] = 0;

    while (!q.empty()) {
        std::string parent_name = q.front();
        q.pop();

        NodeTransform parent_xform = link_world_transforms[parent_name];
        auto child_joints = robot_def.get_child_joints(parent_name);

        for (const auto* joint : child_joints) {
            if (!joint || joint->child_link.empty())
                continue;

            std::string child_name = joint->child_link;
            if (link_to_skeleton_idx.find(child_name) != link_to_skeleton_idx.end())
                continue; // Prevent cyclic loops if any

            // Joint local transform relative to parent
            JPH::Vec3 j_pos(joint->origin_xyz[0], joint->origin_xyz[1], joint->origin_xyz[2]);
            JPH::Quat j_rot = rpy_to_quat(joint->origin_rpy[0], joint->origin_rpy[1], joint->origin_rpy[2]);

            // Calculate child world transform
            NodeTransform child_xform;
            child_xform.position = parent_xform.position + parent_xform.rotation * j_pos;
            child_xform.rotation = (parent_xform.rotation * j_rot).Normalized();

            link_world_transforms[child_name] = child_xform;
            link_to_parent_joint[child_name] = joint;

            int new_idx = static_cast<int>(ordered_link_names.size());
            link_to_skeleton_idx[child_name] = new_idx;
            ordered_link_names.push_back(child_name);
            q.push(child_name);
        }
    }

    // 2. Build Jolt Skeleton & RagdollSettings
    JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton();
    JPH::Ref<JPH::RagdollSettings> ragdoll_settings = new JPH::RagdollSettings();
    ragdoll_settings->mParts.resize(ordered_link_names.size());

    std::unordered_map<std::string, int> joint_to_constraint_idx;
    int constraint_counter = 0;

    for (size_t i = 0; i < ordered_link_names.size(); ++i) {
        const std::string& link_name = ordered_link_names[i];
        const auto* link_def = robot_def.find_link(link_name);
        if (!link_def)
            continue;

        int parent_joint_idx = -1;
        auto p_it = link_to_parent_joint.find(link_name);
        if (p_it != link_to_parent_joint.end()) {
            std::string p_name = p_it->second->parent_link;
            auto s_it = link_to_skeleton_idx.find(p_name);
            if (s_it != link_to_skeleton_idx.end())
                parent_joint_idx = s_it->second;
        }

        skeleton->AddJoint(link_name, parent_joint_idx);

        // Configure Part (BodyCreationSettings)
        auto& part = ragdoll_settings->mParts[i];
        NodeTransform xform = link_world_transforms[link_name];

        part.SetShapeSettings(create_link_shape(*link_def));
        part.mPosition = xform.position;
        part.mRotation = xform.rotation;
        part.mMotionType = (i == 0) ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
        part.mObjectLayer = 1; // LAYER_DYNAMIC
        part.mLinearDamping = 0.5f;
        part.mAngularDamping = 2.0f;
        part.mMaxAngularVelocity = 25.0f;
        part.mMaxLinearVelocity = 20.0f;

        // Mass and Inertia configuration: provide safe minimum inertia to avoid explosive lever-arm torques
        constexpr float k_min_mass = 0.25f;
        constexpr float k_fallback_mass = 0.6f;
        float mass = link_def->inertial.mass > k_min_mass ? link_def->inertial.mass : k_fallback_mass;

        JPH::MassProperties mp;
        mp.mMass = mass;
        constexpr float k_proxy_radius = 0.08f;
        float safe_inertia = std::max(0.005f, 0.4f * mass * k_proxy_radius * k_proxy_radius);
        mp.mInertia = JPH::Mat44::sScale(safe_inertia);
        part.mMassPropertiesOverride = mp;
        part.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;

        // Connect Part to Parent with Constraint
        if (parent_joint_idx >= 0 && p_it != link_to_parent_joint.end()) {
            const auto* joint_def = p_it->second;
            NodeTransform p_xform = link_world_transforms[joint_def->parent_link];

            JPH::RVec3 anchor_pos = xform.position; // Joint origin matches child link frame origin
            JPH::Vec3 raw_axis = xform.rotation * JPH::Vec3(joint_def->axis[0], joint_def->axis[1], joint_def->axis[2]);
            JPH::Vec3 hinge_axis = raw_axis.LengthSq() > 1e-4f ? raw_axis.Normalized() : JPH::Vec3::sAxisZ();

            if (joint_def->type == bud::robots::JointType::Revolute || joint_def->type == bud::robots::JointType::Continuous) {
                JPH::Ref<JPH::HingeConstraintSettings> hinge = new JPH::HingeConstraintSettings();
                hinge->mSpace = JPH::EConstraintSpace::WorldSpace;
                hinge->mPoint1 = anchor_pos;
                hinge->mPoint2 = anchor_pos;
                hinge->mHingeAxis1 = hinge_axis;
                hinge->mHingeAxis2 = hinge_axis;

                // Make normal axis perpendicular to hinge axis
                JPH::Vec3 normal = hinge_axis.GetNormalizedPerpendicular();
                hinge->mNormalAxis1 = normal;
                hinge->mNormalAxis2 = normal;

                if (joint_def->type == bud::robots::JointType::Revolute) {
                    hinge->mLimitsMin = joint_def->limit.lower;
                    hinge->mLimitsMax = joint_def->limit.upper;
                } else {
                    constexpr float pi_val = 3.14159265f;
                    hinge->mLimitsMin = -pi_val;
                    hinge->mLimitsMax = pi_val;
                }

                // Spring & Motor Settings: use FrequencyAndDamping with critical damping ratio = 1.0 (unconditionally stable)
                if (params.enable_motors) {
                    constexpr float k_frequency = 4.0f; // 4 Hz stable, smooth robotics PD control
                    constexpr float k_damping_ratio = 1.0f; // Critical damping (no resonance overshoot)
                    float max_torque = std::clamp(joint_def->limit.effort > 0.0f ? joint_def->limit.effort * 1.5f : 120.0f, 40.0f, 180.0f);

                    hinge->mMotorSettings.mSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, k_frequency, k_damping_ratio);
                    hinge->mMotorSettings.SetTorqueLimits(-max_torque, max_torque);
                    hinge->mMotorSettings.SetForceLimits(-1.0e6f, 1.0e6f);
                }

                part.mToParent = hinge;
            } else {
                // Fixed joint fallback
                JPH::Ref<JPH::FixedConstraintSettings> fixed_c = new JPH::FixedConstraintSettings();
                fixed_c->mSpace = JPH::EConstraintSpace::WorldSpace;
                fixed_c->mAutoDetectPoint = true;
                part.mToParent = fixed_c;
            }
        }
    }

    ragdoll_settings->mSkeleton = skeleton;

    // 3. Apply Havok/Isaac Sim Grade Stabilization & Prioritization
    ragdoll_settings->Stabilize();
    ragdoll_settings->CalculateConstraintPriorities(0);
    ragdoll_settings->CalculateBodyIndexToConstraintIndex();

    // Disable ALL internal robot self-collisions: prevent internal link penetration impulses
    int joint_count = static_cast<int>(ordered_link_names.size());
    JPH::Ref<JPH::GroupFilterTable> group_filter = new JPH::GroupFilterTable(joint_count);
    for (int j1 = 0; j1 < joint_count; ++j1) {
        for (int j2 = j1 + 1; j2 < joint_count; ++j2) {
            group_filter->DisableCollision(j1, j2);
        }
    }
    for (int joint_idx = 0; joint_idx < joint_count; ++joint_idx) {
        auto& part = ragdoll_settings->mParts[joint_idx];
        part.mCollisionGroup.SetSubGroupID(joint_idx);
        part.mCollisionGroup.SetGroupFilter(group_filter);
    }

    // Map joints to their true Jolt Ragdoll constraint indices
    for (const auto& joint : robot_def.joints) {
        auto c_it = link_to_skeleton_idx.find(joint.child_link);
        if (c_it != link_to_skeleton_idx.end()) {
            int body_idx = c_it->second;
            int c_idx = ragdoll_settings->GetConstraintIndexForBodyIndex(body_idx);
            if (c_idx >= 0) {
                joint_to_constraint_idx[joint.name] = c_idx;
                constraint_counter++;
            }
        }
    }

    // 4. Instantiate and Spawn into Simulation
    static uint32_t s_robot_group_counter = 1;
    JPH::CollisionGroup::GroupID group_id = s_robot_group_counter++;

    JPH::Ref<JPH::Ragdoll> ragdoll = ragdoll_settings->CreateRagdoll(group_id, 0, system);
    if (!ragdoll) {
        std::cerr << "[RobotLoader] Failed to create JPH::Ragdoll instance." << std::endl;
        return nullptr;
    }

    ragdoll->AddToPhysicsSystem(params.activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

    std::cout << "[RobotLoader] Successfully spawned robot '" << robot_def.name 
              << "' with " << ordered_link_names.size() << " rigid bodies and " 
              << constraint_counter << " stabilized constraints." << std::endl;

    return std::make_unique<RobotInstance>(robot_def,
                                           ragdoll,
                                           system,
                                           std::move(link_to_skeleton_idx),
                                           std::move(joint_to_constraint_idx));
}

std::unique_ptr<RobotInstance> RobotLoader::spawn_robot_from_file(physics::PhysicsScene& scene,
                                                                  const std::string& robot_file_path,
                                                                  const RobotSpawnParams& params) {
    auto robot_def_opt = bud::robots::RobotDef::load_from_budasset(robot_file_path);
    if (!robot_def_opt) {
        std::cerr << "[RobotLoader] Error: Could not load robot from " << robot_file_path << std::endl;
        return nullptr;
    }

    std::string package_root = std::filesystem::path(robot_file_path).parent_path().string();
    ensure_link_mesh_collisions(*robot_def_opt, package_root);

    return spawn_robot(scene, *robot_def_opt, params);
}

} // namespace bud::robots
