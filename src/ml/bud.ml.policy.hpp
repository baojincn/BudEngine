#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "bud.ml.types.hpp"
#include "bud.ml.tensor.hpp"
#include "bud.ml.backend.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::ml::rl {

// PolicyBase: 抽象策略接口。
// 设计目标：
// - 支持 GPU 零拷贝评估（compute shader path）和 CPU 回退
// - 可以注入或绑定一个 InferBackend 实现（例如 ONNX 占位）
// - 提供 I/O 配置元数据以便与 shader / backend 对齐
class PolicyBase {
public:
    virtual ~PolicyBase() = default;

    // 将推理后端注入到策略（可选）
    virtual void set_backend(std::shared_ptr<ml::InferBackendBase> backend) = 0;

    // 配置输入/输出名与维度（用于绑定或生成 descriptor 布局）
    virtual void configure_io(const std::vector<std::pair<std::string, size_t>>& inputs,
                              const std::vector<std::pair<std::string, size_t>>& outputs) = 0;

    // 观测/动作维度查询
    virtual size_t obs_dim() const = 0;
    virtual size_t action_dim() const = 0;

    // CPU 同步评估（训练/调试或无 GPU 后端的回退）
    virtual void evaluate_cpu(const BatchedObservations& in, BatchedActions& out) = 0;

    // GPU 零拷贝评估：在指定 command buffer/record 回调中被调用，直接使用 GPU 资源
    virtual void evaluate_gpu(const ml::GpuTensor& input, ml::GpuTensor& output,
                              bud::graphics::RHI* rhi, bud::graphics::CommandHandle cmd) = 0;

    // 策略名称（用于日志/调试/资产映射）
    virtual const std::string& name() const = 0;
};

// NOTE: Do not provide an `IPolicy` alias; prefer explicit `PolicyBase` usage across the codebase.

// GPU 批数据小类型（轻量包装，便于在 RenderGraph 中传递）
struct GpuBatchedObservations {
    ml::GpuTensor tensor;
    size_t obs_dim = 0;
    size_t agent_count = 0;
};

struct GpuBatchedActions {
    ml::GpuTensor tensor;
    size_t action_dim = 0;
    size_t agent_count = 0;
};

} // namespace bud::ml::rl
