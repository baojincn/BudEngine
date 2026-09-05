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
        if (pipeline.is_valid()) rhi->destroy_pipeline(pipeline);
        pipeline.reset();
        if (vertex_buffer.is_valid()) rhi->destroy_buffer(vertex_buffer);
        vertex_buffer.reset();
        vertex_capacity = 0;
        if (ubo_buffer.is_valid()) rhi->destroy_buffer(ubo_buffer);
        ubo_buffer.reset();
        if (set_layout) rhi->destroy_descriptor_set_layout(set_layout);
        set_layout = 0;
    }

    void PhysicsDebugPass::update_vertices(const std::vector<PhysicsDebugVertex>& verts) {
        std::lock_guard<std::mutex> lock(vertices_mutex);
        cpu_vertices = verts;
    }

    RGHandle PhysicsDebugPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle /*depth_buffer*/,
                                             const SceneView& view, const RenderConfig& config) {
        if (!pipeline.is_valid()) return {};

        // Called on the render thread while the logic thread may be pushing a new
        // snapshot: take a consistent copy under the lock. The previous snapshot is
        // kept until the logic thread replaces it so the overlay does not flicker
        // when render frames outrun fixed logic steps.
        std::vector<PhysicsDebugVertex> verts;
        {
            std::lock_guard<std::mutex> lock(vertices_mutex);
            if (cpu_vertices.empty()) return {};
            verts = cpu_vertices;
        }

        return rg.add_pass("Physics Debug Pass",
            [=](RGBuilder& builder) {
                builder.write(backbuffer, ResourceState::RenderTarget);
                return backbuffer;
            },
            [=, &rg, this](RHI* rhi, CommandHandle cmd) {
                const uint64_t bytes = static_cast<uint64_t>(verts.size()) * sizeof(PhysicsDebugVertex);

                // cmd_copy_to_buffer() wraps vkCmdUpdateBuffer, which is hard-capped at
                // 64 KiB (VUID-vkCmdUpdateBuffer-dataSize-00037). 388 boxes * 24 verts *
                // 24 bytes is ~218 KiB, so that call was rejected and the buffer kept its
                // undefined contents. Upload by writing into the persistent host mapping
                // instead - the same mechanism the renderer uses for mesh and instance
                // data - and only recreate the buffer when it has to grow.
                if (!vertex_buffer.is_valid() || vertex_capacity < bytes) {
                    if (vertex_buffer.is_valid())
                        rhi->destroy_buffer(vertex_buffer);
                    vertex_capacity = (bytes + 65535ull) & ~65535ull; // grow in 64 KiB steps
                    vertex_buffer = rhi->create_upload_buffer(vertex_capacity);
                }
                if (auto* vb = rhi->get_buffer(vertex_buffer); vb && vb->mapped_ptr) {
                    std::memcpy(vb->mapped_ptr, verts.data(), bytes);
                } else if (bytes <= 65536ull) {
                    rhi->cmd_copy_to_buffer(cmd, vertex_buffer, 0, bytes, verts.data());
                } else {
                    bud::eprint("[PhysicsDebugPass] vertex buffer is not host-mapped and {} bytes "
                                "exceeds the vkCmdUpdateBuffer limit - overlay would be stale.", bytes);
                }

                struct { float mvp[16]; } ubo;
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        ubo.mvp[i * 4 + j] = view.view_proj_matrix[i][j];

                if (auto* ub = rhi->get_buffer(ubo_buffer); ub && ub->mapped_ptr)
                    std::memcpy(ub->mapped_ptr, &ubo, sizeof(ubo));
                else
                    rhi->cmd_copy_to_buffer(cmd, ubo_buffer, 0, sizeof(ubo), &ubo);

                // NOTE: descriptor sets come from a per-frame pool in this RHI, so a set
                // must NOT be cached across frames - a cached handle goes stale and
                // vkCmdBindDescriptorSets rejects it ("Invalid VkDescriptorSet"), which
                // then makes vkCmdDraw fail with "uses set 0 but that set is not bound".
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
                rhi->cmd_bind_vertex_buffer(cmd, vertex_buffer);
                rhi->cmd_bind_descriptor_set(cmd, pipeline, 0, ds);
                rhi->cmd_bind_descriptor_set(cmd, pipeline, 1);
                rhi->cmd_draw(cmd, (uint32_t)verts.size(), 1, 0, 0);
                rhi->cmd_end_render_pass(cmd);

                // Attribute this overlay draw in the HUD stats. get_render_stats() and
                // get_stats() alias the same per-frame struct (cleared in begin_frame), so
                // these show up one frame later, exactly like every other counter here.
                auto& rs = rhi->get_render_stats();
                rs.physics_debug_draw_calls++;
                rs.physics_debug_line_vertices = static_cast<uint32_t>(verts.size());
                rs.physics_debug_boxes = static_cast<uint32_t>(verts.size() / 24); // 12 edges = 24 verts
            });
    }

}