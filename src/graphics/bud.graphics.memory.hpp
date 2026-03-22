
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

		// 1. GPU 专用资源 (Device Local)
		// 用途：Mega-Buffer, 静态 Mesh数据
		virtual BufferHandle alloc_gpu(uint64_t size, ResourceState usage) = 0;

		// 2. 帧临时资源 (Transient)
		// 策略：线性分配
		virtual BufferHandle alloc_frame_transient(uint64_t size, uint64_t alignment) = 0;

		// 3. 数据上传 (Staging Ring)
		// 策略：环形缓冲 (Ring Buffer)，每帧重置
		// 用途：UniformBuffer, UI动态顶点
		virtual BufferHandle alloc_staging(uint64_t size, uint64_t alignment = 256) = 0;

		// 4. 持久映射资源 (Persistent Mapped)
		// 策略：分配后一直保持 Mapped 状态，跨帧复用 (需自行管理内部偏移)
		// 用途：MDI Instance Data, Indirect Commands
		virtual BufferHandle alloc_persistent(uint64_t size, ResourceState usage) = 0;

		// 5. 纹理分配
		virtual Texture* create_texture(const TextureDesc& desc) = 0;

		// 延迟释放：将资源标记为在给定帧安全释放（由后台实现基于 fence 或保留帧数回收）
		virtual void defer_free(const BufferHandle& handle, uint32_t frame_index) = 0;

		// 可选：延迟释放纹理句柄
		virtual void defer_free(Texture* texture, uint32_t frame_index) = 0;
	};
}
