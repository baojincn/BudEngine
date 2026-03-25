#include "src/graphics/bud.ml_perception.hpp"
#include "src/io/bud.io.hpp"
#include <iostream>

namespace bud::graphics {
    void DepthDownsamplePass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
        if (!rhi || !asset_manager) {
            std::string err = std::format("DepthDownsamplePass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return;
#endif
        }
        
        load_shaders_async(asset_manager, { "src/shaders/ml_depth_downsample.comp.spv" }, [this, rhi](const auto& shaders) {
            ComputePipelineDesc desc{};
            desc.cs.code = shaders[0];
            pipeline = rhi->create_compute_pipeline(desc);
            if (pipeline) {
                std::cout << "[DepthDownsamplePass] Shader loaded and pipeline created.\n";
            }
        });
    }

    RGHandle DepthDownsamplePass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, uint32_t target_width, uint32_t target_height) {
        if (!pipeline) {
            std::string err = "DepthDownsamplePass::add_to_graph called with null pipeline";
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return RGHandle{};
#endif
        }

        RGHandle output_tex;

        rg.add_pass("Depth Downsample Pass",
            [&](RGBuilder& builder) {
                TextureDesc desc{};
                desc.width = target_width;
                desc.height = target_height;
                desc.format = TextureFormat::R32_FLOAT;
                desc.is_storage = true;
                
                output_tex = builder.create("DownsampledDepth", desc);

                builder.read(depth_buffer, ResourceState::ShaderResource);
                builder.write(output_tex, ResourceState::UnorderedAccess);
            },
            [this, rg_ptr = &rg, depth_buffer, output_tex, target_width, target_height](RHI* rhi, CommandHandle cmd) {
                if (!pipeline) return;
                rhi->cmd_bind_pipeline(cmd, pipeline);

                rhi->cmd_bind_compute_texture(cmd, pipeline, 0, rg_ptr->get_texture(depth_buffer), 0, false, false);
                rhi->cmd_bind_compute_texture(cmd, pipeline, 1, rg_ptr->get_texture(output_tex), 0, true, false);

                uint32_t groups_x = (target_width + 7) / 8;
                uint32_t groups_y = (target_height + 7) / 8;
                rhi->cmd_dispatch(cmd, groups_x, groups_y, 1);
            }
        );

        return output_tex;
    }
}
