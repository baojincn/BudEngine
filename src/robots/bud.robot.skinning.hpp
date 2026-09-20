#pragma once

#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/io/bud.io.hpp"

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <glm/glm.hpp>

namespace bud::robots {

struct BudSkinVertex {
    float pos[3];
    float bone_id;
    float normal[3];
    float pad;
};

class RobotSkinningSystem {
public:
    RobotSkinningSystem() = default;
    ~RobotSkinningSystem() = default;

    bool init(bud::graphics::RHI* rhi, bud::io::AssetManager* asset_manager);
    void shutdown(bud::graphics::RHI* rhi);

    bool register_robot_skin(bud::graphics::RHI* rhi,
                             const std::string& skin_file_path,
                             uint32_t base_vertex_offset);

    void update_bone_matrices(bud::graphics::RHI* rhi,
                              const std::vector<glm::mat4>& bone_matrices);

    void dispatch_skinning(bud::graphics::RHI* rhi,
                           bud::graphics::CommandHandle cmd,
                           bud::graphics::BufferHandle mega_vertex_buffer);

    bool is_registered() const {
        return m_registered && m_pipeline.is_valid();
    }

    uint32_t get_bone_count() const {
        return static_cast<uint32_t>(m_bone_names.size());
    }

    const std::vector<std::string>& get_bone_names() const {
        return m_bone_names;
    }

    const std::vector<glm::mat4>& get_rest_xforms() const {
        return m_rest_xforms;
    }

    uint32_t get_vertex_count() const {
        return m_vertex_count;
    }

    uint32_t get_base_vertex_offset() const {
        return m_base_vertex_offset;
    }

    void set_base_vertex_offset(uint32_t offset) {
        m_base_vertex_offset = offset;
    }

    void set_active(bool active) {
        m_active = active;
    }

    bool is_active() const {
        return m_active;
    }

private:
    bud::graphics::PipelineHandle m_pipeline;
    bud::graphics::BufferHandle m_rest_vertex_buffer;
    bud::graphics::BufferHandle m_bone_matrix_buffer;

    std::vector<std::string> m_bone_names;
    std::vector<glm::mat4> m_rest_xforms;
    std::vector<glm::mat4> m_current_matrices;

    uint32_t m_vertex_count = 0;
    uint32_t m_base_vertex_offset = 0;
    bool m_registered = false;
    bool m_active = true;
    std::mutex m_mutex;
};

} // namespace bud::robots
