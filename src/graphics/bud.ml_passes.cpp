#include "src/graphics/bud.ml_passes.hpp"
#include "src/io/bud.io.hpp"
#include <iostream>

namespace bud::graphics {
    void NeuralOccluderPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
        if (!rhi || !asset_manager) {
            std::string err = std::format("NeuralOccluderPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return;
#endif
        }
        
        load_shaders_async(asset_manager, { "src/shaders/ml_identity.comp.spv" }, [this, rhi](const auto& shaders) {
            ComputePipelineDesc desc{};
            desc.cs.code = shaders[0];
            pipeline = rhi->create_compute_pipeline(desc);
            if (pipeline) {
                std::cout << "[NeuralOccluderPass] Shader loaded and pipeline created.\n";
            }
        });
    }

    void NeuralOccluderPass::add_to_graph(RenderGraph& rg, RGHandle input_buffer, RGHandle output_buffer, uint32_t count) {
        if (!pipeline) {
            std::string err = "NeuralOccluderPass::add_to_graph called with null pipeline";
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return;
#endif
        }

        rg.add_pass("Neural Occluder Pass",
            [&](RGBuilder& builder) {
                builder.read(input_buffer, ResourceState::ShaderResource);
                builder.write(output_buffer, ResourceState::UnorderedAccess);
                builder.set_side_effect(true); // Prevent culling
            },
            [=, rg_ptr = &rg](RHI* rhi, CommandHandle cmd) {
                if (!pipeline) return;
                rhi->cmd_bind_pipeline(cmd, pipeline);

                rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, rg_ptr->get_buffer(input_buffer));
                rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, rg_ptr->get_buffer(output_buffer));

                uint32_t groups = (count + 63) / 64;
                rhi->cmd_dispatch(cmd, groups, 1, 1);

                // Update ML stats
                auto& stats = rhi->get_render_stats();
                stats.occluder_count += count;
                // TRIANGLES TODO: We need the triangle count of selected occluders.
                // For now, let's assume an average or track it elsewhere.
            }
        );
    }
}
