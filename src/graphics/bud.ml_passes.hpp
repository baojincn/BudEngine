#pragma once
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

namespace bud::io { class AssetManager; }

namespace bud::graphics {
    // NeuralOccluderPass: 使用神经网络（或 compute shader）选择用于 z-prepass 的遮挡体
    // 与 HeuristicOccluderPass 并列，用于对比或替代基于投影面积的启发式选择。
    class NeuralOccluderPass : public RenderPass {
    public:
        void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
        void add_to_graph(RenderGraph& rg, RGHandle input_buffer, RGHandle output_buffer, uint32_t count);
    };
}
