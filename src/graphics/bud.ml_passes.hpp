#pragma once
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::io { class AssetManager; }

namespace bud::graphics {
    class IdentityComputePass {
        void* pipeline = nullptr;
    public:
        void init(RHI* rhi, bud::io::AssetManager* asset_manager);
        void add_to_graph(RenderGraph& rg, RGHandle input_buffer, RGHandle output_buffer, uint32_t count);
    };
}
