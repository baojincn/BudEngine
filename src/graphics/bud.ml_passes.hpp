#pragma once
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

namespace bud::io { class AssetManager; }

namespace bud::graphics {
    class OccluderSelectionPass : public RenderPass {
    public:
        void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
        void add_to_graph(RenderGraph& rg, RGHandle input_buffer, RGHandle output_buffer, uint32_t count);
    };
}
