

#pragma once

#include <functional>
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.memory.hpp"

namespace bud::graphics {

	// 资源池接口
	// RenderGraph 在 Compile 阶段会计算好所有的 Transient 资源需求
	// 然后调用这个池子去“变”出实际的物理资源
	class ResourcePool {
	public:
		virtual ~ResourcePool() = default;

		// Handle-based texture pool interface
		virtual TextureHandle acquire_texture(const TextureDesc& desc) = 0;
		virtual void release_texture(TextureHandle handle) = 0;
		virtual Texture* get_texture(TextureHandle handle) = 0;
		virtual const Texture* get_texture(TextureHandle handle) const = 0;
		virtual TextureDesc get_texture_desc(TextureHandle handle) const = 0;

		// Handle-based buffer pool interface
		virtual BufferHandle acquire_buffer(const BufferDesc& desc) = 0;
		virtual void release_buffer(BufferHandle handle) = 0;
		virtual Buffer* get_buffer(BufferHandle handle) = 0;
		virtual const Buffer* get_buffer(BufferHandle handle) const = 0;
		virtual BufferDesc get_buffer_desc(BufferHandle handle) const = 0;

		// 帧结束清理 (决定是否真的销毁过老的资源)
		virtual void tick() = 0;
	};
}
