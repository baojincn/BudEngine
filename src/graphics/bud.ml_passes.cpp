#include "src/graphics/bud.ml_passes.hpp"
#include "src/io/bud.io.hpp"
#include <iostream>
#include <thread>

namespace bud::graphics {

    void IdentityComputePass::init(RHI* rhi, bud::io::AssetManager* asset_manager) {
        asset_manager->load_file_async("src/shaders/ml_identity.comp.spv", [this, rhi](std::vector<char> code) {
            if (code.empty()) {
                std::cerr << "[IdentityComputePass] Failed to load shader.\n";
                return;
            }

            ComputePipelineDesc desc{};
            desc.cs.code = std::move(code);
            pipeline = rhi->create_compute_pipeline(desc);

            std::cout << "[IdentityComputePass] Shader loaded and pipeline created.\n";
        });
    }

    void IdentityComputePass::add_to_graph(RenderGraph& rg, RGHandle input_buffer, RGHandle output_buffer, uint32_t count) {
        if (!pipeline) return;

        rg.add_pass("Identity Compute Pass",
            [&](RGBuilder& builder) {
                builder.read(input_buffer, ResourceState::ShaderResource);
                builder.write(output_buffer, ResourceState::UnorderedAccess);
                builder.set_side_effect(true); // Prevent culling during graph compilation
            },
            [=, &rg](RHI* rhi, CommandHandle cmd) {
                rhi->cmd_bind_pipeline(cmd, pipeline);

                // Bind resources through RHI abstraction
                rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, rg.get_buffer(input_buffer));
                rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, rg.get_buffer(output_buffer));

                uint32_t groups = (count + 63) / 64;
                rhi->cmd_dispatch(cmd, groups, 1, 1);
            }
        );
    }
}
