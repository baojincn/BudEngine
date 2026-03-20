
#include <cstdint>

#pragma once

#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics {

	class Allocator {
	public:
		virtual ~Allocator() = default;

		virtual void init() = 0;
		virtual void cleanup() = 0;

		// 帧开始/结束回调 (用于翻转 RingBuffer，重置 Linear Allocator)
		virtual void on_frame_begin(uint32_t frame_index) = 0;

		// 静态资源将由具体的 RHI 后端直接通过 VMA/D3DAliasing 创建，不再通过 Allocator 的跨 API 抽象暴露


		// 2. 帧临时资源 (Transient)
		// 策略：线性分配 (Linear Bump Pointer)，速度极快，无碎片，无需 Free
		// 用途：RenderGraph 的 RenderTarget, 临时 Buffer
		virtual BufferHandle alloc_frame_transient(uint64_t size, uint64_t alignment) = 0;

		// 3. 数据上传 (Staging)
		// 策略：环形缓冲 (Ring Buffer) 或 每帧线性分配
		// 用途：UniformBuffer, 动态顶点
		virtual BufferHandle alloc_staging(uint64_t size, uint64_t alignment) = 0;
	};
}
