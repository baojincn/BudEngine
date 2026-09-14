#include "src/robots/bud.robot.skinning.hpp"
#include <fstream>
#include <iostream>
#include <cstring>

namespace bud::robots {

constexpr uint32_t k_max_bones = 64;

bool RobotSkinningSystem::init(bud::graphics::RHI* rhi, bud::io::AssetManager* asset_manager) {
    if (!rhi || !asset_manager)
        return false;

    asset_manager->load_file_async("src/shaders/robot_skinning.comp.spv", [this, rhi](std::vector<char> data) {
        if (data.empty())
            return;

        bud::graphics::ComputePipelineDesc desc{};
        desc.cs.code = std::move(data);
        desc.layout_kind = bud::graphics::ComputePipelineDesc::LayoutKind::RobotSkinning;
        m_pipeline = rhi->create_compute_pipeline(desc);
        std::cout << "[RobotSkinningSystem] Loaded robot_skinning.comp pipeline." << std::endl;
    });

    return true;
}

void RobotSkinningSystem::shutdown(bud::graphics::RHI* rhi) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!rhi)
        return;

    if (m_pipeline.is_valid()) {
        rhi->destroy_pipeline(m_pipeline);
        m_pipeline.reset();
    }
    if (m_rest_vertex_buffer.is_valid()) {
        rhi->destroy_buffer(m_rest_vertex_buffer);
        m_rest_vertex_buffer.reset();
    }
    if (m_bone_matrix_buffer.is_valid()) {
        rhi->destroy_buffer(m_bone_matrix_buffer);
        m_bone_matrix_buffer.reset();
    }

    m_registered = false;
    m_bone_names.clear();
    m_rest_xforms.clear();
    m_current_matrices.clear();
    m_vertex_count = 0;
}

#include "src/core/bud.asset.types.hpp"

bool RobotSkinningSystem::register_robot_skin(bud::graphics::RHI* rhi,
                                             const std::string& skin_file_path,
                                             uint32_t base_vertex_offset) {
    if (!rhi)
        return false;

    if (!skin_file_path.ends_with(".budasset")) {
        std::cerr << "[RobotSkinningSystem] Skin file must be a .budasset: " << skin_file_path << std::endl;
        return false;
    }

    std::ifstream file(skin_file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[RobotSkinningSystem] Failed to open robot asset: " << skin_file_path << std::endl;
        return false;
    }

    size_t sz = static_cast<size_t>(file.tellg());
    file.seekg(0);
    if (sz < sizeof(bud::asset::BudAssetHeader))
        return false;

    std::vector<uint8_t> buf(sz);
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz)))
        return false;

    const auto* h = reinterpret_cast<const bud::asset::BudAssetHeader*>(buf.data());
    if (h->magic != bud::asset::BUD_ASSET_MAGIC ||
        h->chunk_table_offset + h->chunk_count * sizeof(bud::asset::AssetChunkEntry) > sz)
        return false;

    uint32_t bone_count = 0;
    uint32_t vertex_count = 0;
    std::vector<std::string> bone_names;
    std::vector<glm::mat4> rest_xforms;
    std::vector<BudSkinVertex> vertices;
    bool loaded = false;

    const auto* chunks = reinterpret_cast<const bud::asset::AssetChunkEntry*>(buf.data() + h->chunk_table_offset);
    for (uint32_t c = 0; c < h->chunk_count; ++c) {
        if (chunks[c].chunk_type != static_cast<uint32_t>(bud::asset::AssetChunkType::Skinning) ||
            chunks[c].offset + chunks[c].size > sz) {
            continue;
        }

        const uint8_t* ptr = buf.data() + chunks[c].offset;
        const uint8_t* end = ptr + chunks[c].size;
        if (ptr + sizeof(bone_count) + sizeof(vertex_count) > end)
            return false;

        std::memcpy(&bone_count, ptr, sizeof(bone_count)); ptr += sizeof(bone_count);
        std::memcpy(&vertex_count, ptr, sizeof(vertex_count)); ptr += sizeof(vertex_count);

        const size_t bone_data_size = static_cast<size_t>(bone_count) * (64 + sizeof(float) * 16);
        const size_t vertex_data_size = static_cast<size_t>(vertex_count) * sizeof(BudSkinVertex);
        if (bone_data_size > static_cast<size_t>(end - ptr) ||
            vertex_data_size > static_cast<size_t>(end - ptr) - bone_data_size) {
            return false;
        }

        bone_names.reserve(bone_count);
        rest_xforms.reserve(bone_count);
        for (uint32_t b = 0; b < bone_count; ++b) {
            char name_buf[64] = { 0 };
            std::memcpy(name_buf, ptr, sizeof(name_buf)); ptr += sizeof(name_buf);
            bone_names.emplace_back(name_buf);

            glm::mat4 m(1.0f);
            std::memcpy(&m[0][0], ptr, sizeof(float) * 16); ptr += sizeof(float) * 16;
            rest_xforms.push_back(m);
        }

        vertices.resize(vertex_count);
        std::memcpy(vertices.data(), ptr, vertex_data_size);
        loaded = true;
        break;
    }

    if (!loaded) {
        std::cerr << "[RobotSkinningSystem] No skinning chunk found in: " << skin_file_path << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_bone_names = std::move(bone_names);
    m_rest_xforms = std::move(rest_xforms);

    m_vertex_count = vertex_count;
    m_base_vertex_offset = base_vertex_offset;

    // Allocate GPU storage buffer for rest vertices
    uint64_t v_bytes = static_cast<uint64_t>(m_vertex_count) * sizeof(BudSkinVertex);
    m_rest_vertex_buffer = rhi->create_gpu_buffer(v_bytes, bud::graphics::ResourceState::ShaderResource);

    auto staging_v = rhi->create_upload_buffer(v_bytes);
    if (auto* b = rhi->get_buffer(staging_v); b && b->mapped_ptr) {
        std::memcpy(b->mapped_ptr, vertices.data(), v_bytes);
    }
    rhi->copy_buffer_immediate(staging_v, m_rest_vertex_buffer, v_bytes);
    rhi->destroy_buffer(staging_v);

    // Allocate GPU storage buffer for bone matrices
    uint64_t m_bytes = k_max_bones * sizeof(glm::mat4);
    m_bone_matrix_buffer = rhi->create_gpu_buffer(m_bytes, bud::graphics::ResourceState::ShaderResource);

    std::vector<glm::mat4> init_matrices(k_max_bones, glm::mat4(1.0f));
    auto staging_m = rhi->create_upload_buffer(m_bytes);
    if (auto* b = rhi->get_buffer(staging_m); b && b->mapped_ptr) {
        std::memcpy(b->mapped_ptr, init_matrices.data(), m_bytes);
    }
    rhi->copy_buffer_immediate(staging_m, m_bone_matrix_buffer, m_bytes);
    rhi->destroy_buffer(staging_m);

    m_registered = true;
    std::cout << "[RobotSkinningSystem] Registered skin with " << bone_count
              << " bones and " << m_vertex_count << " vertices (base offset: "
              << m_base_vertex_offset << ")." << std::endl;
    return true;
}

void RobotSkinningSystem::update_bone_matrices(bud::graphics::RHI* rhi,
                                              const std::vector<glm::mat4>& bone_matrices) {
    if (!m_registered || !rhi || !m_bone_matrix_buffer.is_valid())
        return;

    std::vector<glm::mat4> upload_mats(k_max_bones, glm::mat4(1.0f));
    size_t count = std::min(bone_matrices.size(), static_cast<size_t>(k_max_bones));
    for (size_t i = 0; i < count; ++i) {
        upload_mats[i] = bone_matrices[i];
    }

    uint64_t bytes = k_max_bones * sizeof(glm::mat4);
    auto staging = rhi->create_upload_buffer(bytes);
    if (auto* b = rhi->get_buffer(staging); b && b->mapped_ptr) {
        std::memcpy(b->mapped_ptr, upload_mats.data(), bytes);
    }
    rhi->copy_buffer_immediate(staging, m_bone_matrix_buffer, bytes);
    rhi->destroy_buffer(staging);
}

void RobotSkinningSystem::dispatch_skinning(bud::graphics::RHI* rhi,
                                           bud::graphics::CommandHandle cmd,
                                           bud::graphics::BufferHandle mega_vertex_buffer) {
    if (!m_active || !m_registered || !m_pipeline.is_valid() || !m_rest_vertex_buffer.is_valid() || !mega_vertex_buffer.is_valid())
        return;

    rhi->cmd_bind_pipeline(cmd, m_pipeline);
    rhi->cmd_bind_storage_buffer(cmd, m_pipeline, 0, m_rest_vertex_buffer);
    rhi->cmd_bind_storage_buffer(cmd, m_pipeline, 1, m_bone_matrix_buffer);
    rhi->cmd_bind_storage_buffer(cmd, m_pipeline, 2, mega_vertex_buffer);

    struct PushConstants {
        uint32_t vertex_count;
        uint32_t base_vertex_offset;
    } pc { m_vertex_count, m_base_vertex_offset };

    rhi->cmd_push_constants(cmd, m_pipeline, sizeof(pc), &pc);

    uint32_t groups_x = (m_vertex_count + 63) / 64;
    rhi->cmd_dispatch(cmd, groups_x, 1, 1);
}

} // namespace bud::robots
