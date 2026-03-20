#pragma once
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

namespace bud::io { class AssetManager; }

namespace bud::graphics {
    class DepthDownsamplePass : public RenderPass {
    public:
        void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
        RGHandle add_to_graph(RenderGraph& rg, RGHandle depth_buffer, uint32_t target_width, uint32_t target_height);
    };
}
