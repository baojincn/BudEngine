#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

namespace bud::graphics {

    void ClothDebugPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
        if (!rhi || !asset_manager) return;
        std::vector<DescriptorBinding> bindings = {
            {0, DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, SHADER_STAGE_VERTEX_BIT},
            {1, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_VERTEX_BIT}, // cloth particles
            {2, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_VERTEX_BIT}, // cloth constraints
        };
        set_layout = rhi->create_descriptor_set_layout(bindings);
        ubo_buffer = rhi->create_upload_buffer(64);

        load_shaders_async(asset_manager, { "src/shaders/cloth_debug.vert.spv", "src/shaders/cloth_debug.frag.spv" },
            [this, rhi, config](const auto& shaders) {
                GraphicsPipelineDesc desc;
                desc.vs.code = shaders[0];
                desc.fs.code = shaders[1];
                desc.depth_test = true;
                desc.depth_write = false;
                desc.cull_mode = CullMode::None;
                desc.topology = PrimitiveTopology::LineList;
                desc.color_attachment_format = TextureFormat::BGRA8_SRGB;
                desc.vertex_layout = VertexLayoutType::NoVertexInput; // pull-model: VS fetches from SSBOs
                desc.custom_set_layouts = { set_layout };
                pipeline = rhi->create_graphics_pipeline(desc);
                if (pipeline.is_valid())
                    bud::print("[ClothDebugPass] Pipeline created.");
            });
    }

    void ClothDebugPass::shutdown(RHI* rhi) {
        if (pipeline.is_valid()) rhi->destroy_pipeline(pipeline);
        pipeline.reset();
        if (ubo_buffer.is_valid()) rhi->destroy_buffer(ubo_buffer);
        ubo_buffer.reset();
        if (set_layout) rhi->destroy_descriptor_set_layout(set_layout);
        set_layout = 0;
    }

    RGHandle ClothDebugPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle /*depth_buffer*/,
                                          const SceneView& view, const RenderConfig& config,
                                          BufferHandle gpu_particles, BufferHandle gpu_constraints, uint32_t constraint_count) {
        if (!pipeline.is_valid() || constraint_count == 0)
            return {};

        return rg.add_pass("Cloth Debug Pass",
            [=](RGBuilder& builder) {
                builder.write(backbuffer, ResourceState::RenderTarget);
                return backbuffer;
            },
            [=, &rg, this](RHI* rhi, CommandHandle cmd) {
                // View projection via the persistent host mapping (same pattern as
                // PhysicsDebugPass).
                struct { float mvp[16]; } ubo;
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        ubo.mvp[i * 4 + j] = view.view_proj_matrix[i][j];

                if (auto* ub = rhi->get_buffer(ubo_buffer); ub && ub->mapped_ptr)
                    std::memcpy(ub->mapped_ptr, &ubo, sizeof(ubo));
                else
                    rhi->cmd_copy_to_buffer(cmd, ubo_buffer, 0, sizeof(ubo), &ubo);

                // Descriptor sets come from a per-frame pool - never cache across frames.
                uint64_t ds = rhi->create_descriptor_set(set_layout);
                rhi->update_descriptor_set_buffer(ds, 0, ubo_buffer, DESCRIPTOR_TYPE_UNIFORM_BUFFER);
                rhi->update_descriptor_set_buffer(ds, 1, gpu_particles, DESCRIPTOR_TYPE_STORAGE_BUFFER);
                rhi->update_descriptor_set_buffer(ds, 2, gpu_constraints, DESCRIPTOR_TYPE_STORAGE_BUFFER);

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
                // Pull-model: 2 vertices per constraint, positions fetched in the VS.
                rhi->cmd_draw(cmd, constraint_count * 2u, 1u, 0u, 0u);
                rhi->cmd_end_render_pass(cmd);
            });
    }

} // namespace bud::graphics
