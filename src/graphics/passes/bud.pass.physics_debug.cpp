#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

#include <cstring>

namespace bud::graphics {

    void PhysicsDebugPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
        if (!rhi || !asset_manager) return;
        std::vector<DescriptorBinding> bindings = {
            {0, DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, SHADER_STAGE_VERTEX_BIT},
        };
        set_layout = rhi->create_descriptor_set_layout(bindings);
        ubo_buffer = rhi->create_upload_buffer(64);
        has_ubo = true;

        load_shaders_async(asset_manager, { "src/shaders/physics_debug.vert.spv", "src/shaders/physics_debug.frag.spv" },
            [this, rhi, config](const auto& shaders) {
                GraphicsPipelineDesc desc;
                desc.vs.code = shaders[0];
                desc.fs.code = shaders[1];
                desc.depth_test = true;
                desc.depth_write = false;
                desc.cull_mode = CullMode::None;
                desc.topology = PrimitiveTopology::LineList;
                desc.color_attachment_format = TextureFormat::BGRA8_SRGB;
                desc.vertex_layout = VertexLayoutType::DebugLine;
                desc.custom_set_layouts = { set_layout };
                pipeline = rhi->create_graphics_pipeline(desc);
                if (pipeline.is_valid())
                    bud::print("[PhysicsDebugPass] Pipeline created.");
            });
    }

    void PhysicsDebugPass::shutdown(RHI* rhi) {
        if (pipeline.is_valid())
            rhi->destroy_pipeline(pipeline);
        pipeline.reset();
        if (vertex_buffer.is_valid())
            rhi->destroy_buffer(vertex_buffer);
        vertex_buffer.reset();
        vertex_capacity = 0;
        if (static_vertex_buffer.is_valid())
            rhi->destroy_buffer(static_vertex_buffer);
        static_vertex_buffer.reset();
        static_vertex_capacity = 0;
        static_vertex_count = 0;
        if (ubo_buffer.is_valid())
            rhi->destroy_buffer(ubo_buffer);
        ubo_buffer.reset();
        if (set_layout)
            rhi->destroy_descriptor_set_layout(set_layout);
        set_layout = 0;
    }

    void PhysicsDebugPass::set_static_vertices(const std::vector<PhysicsDebugVertex>& verts) {
        std::lock_guard<std::mutex> lock(vertices_mutex);
        pending_static_vertices = verts;
        static_vertices_dirty = true;
    }

    void PhysicsDebugPass::update_vertices(const std::vector<PhysicsDebugVertex>& verts) {
        std::lock_guard<std::mutex> lock(vertices_mutex);
        cpu_vertices = verts;
    }

    RGHandle PhysicsDebugPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle /*depth_buffer*/,
                                             const SceneView& view, const RenderConfig& config) {
        if (!pipeline.is_valid())
            return {};

        std::vector<PhysicsDebugVertex> dyn_verts;
        std::vector<PhysicsDebugVertex> new_static_verts;
        bool upload_static = false;
        {
            std::lock_guard<std::mutex> lock(vertices_mutex);
            dyn_verts = cpu_vertices;
            if (static_vertices_dirty) {
                new_static_verts = std::move(pending_static_vertices);
                upload_static = true;
                static_vertices_dirty = false;
            }
        }

        if (dyn_verts.empty() && static_vertex_count == 0 && !upload_static)
            return {};

        return rg.add_pass("Physics Debug Pass",
            [=](RGBuilder& builder) {
                builder.write(backbuffer, ResourceState::RenderTarget);
                return backbuffer;
            },
            [=, &rg, this, dyn_verts = std::move(dyn_verts), new_static_verts = std::move(new_static_verts)](RHI* rhi, CommandHandle cmd) {
                if (upload_static) {
                    if (new_static_verts.empty()) {
                        if (static_vertex_buffer.is_valid())
                            rhi->destroy_buffer(static_vertex_buffer);
                        static_vertex_buffer.reset();
                        static_vertex_capacity = 0;
                        static_vertex_count = 0;
                    }
                    else {
                        const uint64_t s_bytes = static_cast<uint64_t>(new_static_verts.size()) * sizeof(PhysicsDebugVertex);
                        if (!static_vertex_buffer.is_valid() || static_vertex_capacity < s_bytes) {
                            if (static_vertex_buffer.is_valid())
                                rhi->destroy_buffer(static_vertex_buffer);
                            static_vertex_capacity = (s_bytes + 65535ull) & ~65535ull;
                            static_vertex_buffer = rhi->create_upload_buffer(static_vertex_capacity);
                        }
                        if (auto* sb = rhi->get_buffer(static_vertex_buffer); sb && sb->mapped_ptr)
                            std::memcpy(sb->mapped_ptr, new_static_verts.data(), s_bytes);
                        else if (s_bytes <= 65536ull)
                            rhi->cmd_copy_to_buffer(cmd, static_vertex_buffer, 0, s_bytes, new_static_verts.data());
                        static_vertex_count = static_cast<uint32_t>(new_static_verts.size());
                    }
                }

                if (!dyn_verts.empty()) {
                    const uint64_t d_bytes = static_cast<uint64_t>(dyn_verts.size()) * sizeof(PhysicsDebugVertex);
                    if (!vertex_buffer.is_valid() || vertex_capacity < d_bytes) {
                        if (vertex_buffer.is_valid())
                            rhi->destroy_buffer(vertex_buffer);
                        vertex_capacity = (d_bytes + 65535ull) & ~65535ull;
                        vertex_buffer = rhi->create_upload_buffer(vertex_capacity);
                    }
                    if (auto* vb = rhi->get_buffer(vertex_buffer); vb && vb->mapped_ptr)
                        std::memcpy(vb->mapped_ptr, dyn_verts.data(), d_bytes);
                    else if (d_bytes <= 65536ull)
                        rhi->cmd_copy_to_buffer(cmd, vertex_buffer, 0, d_bytes, dyn_verts.data());
                }

                struct { float mvp[16]; } ubo;
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        ubo.mvp[i * 4 + j] = view.view_proj_matrix[i][j];

                if (auto* ub = rhi->get_buffer(ubo_buffer); ub && ub->mapped_ptr)
                    std::memcpy(ub->mapped_ptr, &ubo, sizeof(ubo));
                else
                    rhi->cmd_copy_to_buffer(cmd, ubo_buffer, 0, sizeof(ubo), &ubo);

                uint64_t ds = rhi->create_descriptor_set(set_layout);
                rhi->update_descriptor_set_buffer(ds, 0, ubo_buffer, DESCRIPTOR_TYPE_UNIFORM_BUFFER);

                RenderPassBeginInfo rpi;
                rpi.color_attachments.push_back(rg.get_texture(backbuffer));
                rpi.clear_color = false;
                rpi.render_width = view.viewport_width;
                rpi.render_height = view.viewport_height;

                rhi->cmd_begin_render_pass(cmd, rpi);
                rhi->cmd_bind_pipeline(cmd, pipeline);
                rhi->cmd_set_viewport(cmd, (float)view.viewport_width, (float)view.viewport_height);
                rhi->cmd_set_scissor(cmd, view.viewport_width, view.viewport_height);
                rhi->cmd_bind_descriptor_set(cmd, pipeline, 0, ds);
                rhi->cmd_bind_descriptor_set(cmd, pipeline, 1);

                if (static_vertex_count > 0 && static_vertex_buffer.is_valid()) {
                    rhi->cmd_bind_vertex_buffer(cmd, static_vertex_buffer);
                    rhi->cmd_draw(cmd, static_vertex_count, 1, 0, 0);
                }

                if (!dyn_verts.empty() && vertex_buffer.is_valid()) {
                    rhi->cmd_bind_vertex_buffer(cmd, vertex_buffer);
                    rhi->cmd_draw(cmd, static_cast<uint32_t>(dyn_verts.size()), 1, 0, 0);
                }

                rhi->cmd_end_render_pass(cmd);

                auto& rs = rhi->get_render_stats();
                uint32_t draws = 0;
                if (static_vertex_count > 0 && static_vertex_buffer.is_valid())
                    draws++;
                if (!dyn_verts.empty() && vertex_buffer.is_valid())
                    draws++;
                rs.physics_debug_draw_calls += draws;
                uint32_t total_vtx = static_vertex_count + static_cast<uint32_t>(dyn_verts.size());
                rs.physics_debug_line_vertices = total_vtx;
                rs.physics_debug_boxes = total_vtx / 24;
            });
    }

}