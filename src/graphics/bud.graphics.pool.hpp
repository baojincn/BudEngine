

#include <functional>

#pragma once

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.memory.hpp"

namespace bud::graphics {

	// 资源池接口
	// RenderGraph 在 Compile 阶段会计算好所有的 Transient 资源需求
	// 然后调用这个池子去“变”出实际的物理资源
	class ResourcePool {
	public:
		virtual ~ResourcePool() = default;

		// 核心接口：给我一个描述，还我一个 handle (不分配显存，或者复用已有)
		virtual Texture* acquire_texture(const TextureDesc& desc) = 0;

		// 归还资源
		virtual void release_texture(Texture* texture) = 0;

		// 帧结束清理 (决定是否真的销毁过老的资源)
		virtual void tick() = 0;
	};
}
