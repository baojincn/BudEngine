module;
#include <cstdint>

export module bud.graphics.memory;

import bud.graphics.defs;
import bud.graphics.types;

export namespace bud::graphics {

	class Allocator {
	public:
		virtual ~Allocator() = default;

		virtual void init() = 0;
		virtual void cleanup() = 0;

		// 帧开始/结束回调 (用于翻转 RingBuffer，重置 Linear Allocator)
		virtual void on_frame_begin(uint32_t frame_index) = 0;

		// --- 1. 长期资源 (Static) ---
		// 策略：直接申请或从空闲链表(FreeList)中拿
		virtual MemoryBlock alloc_static(uint64_t size, uint64_t alignment, uint32_t memory_type_bits, MemoryUsage usage) = 0;
		virtual void free(const MemoryBlock& block) = 0;

		// --- 2. 帧临时资源 (Transient) ---
		// 策略：线性分配 (Linear Bump Pointer)，速度极快，无碎片，无需 Free
		// 用途：RenderGraph 的 RenderTarget, 临时 Buffer
		virtual MemoryBlock alloc_frame_transient(uint64_t size, uint64_t alignment, uint32_t memory_type_bits) = 0;

		// --- 3. 数据上传 (Staging) ---
		// 策略：环形缓冲 (Ring Buffer) 或 每帧线性分配
		// 用途：UniformBuffer, 动态顶点
		virtual MemoryBlock alloc_staging(uint64_t size, uint64_t alignment) = 0;
	};
}
