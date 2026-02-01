#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>  // for unique_ptr
#include <utility> // for move

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.pool.hpp"
#include "src/graphics/bud.graphics.rhi.hpp" // For ResourcePool base
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"

namespace bud::graphics::vulkan {

	class VulkanResourcePool : public ResourcePool {
	public:
		VulkanResourcePool(VkDevice device, VulkanMemoryAllocator* allocator);
		~VulkanResourcePool();

		void cleanup();
		Texture* acquire_texture(const TextureDesc& desc) override;
		void release_texture(Texture* texture) override;
		void tick() override;

	private:
		VkDevice device;
		VulkanMemoryAllocator* allocator;

		using TexturePtr = std::unique_ptr<VulkanTexture>;
		std::unordered_map<size_t, std::vector<TexturePtr>> image_pool;
		std::unordered_set<VulkanTexture*> acquired_textures;

		size_t hash_desc(const TextureDesc& desc);
		void destroy_vulkan_objects(VulkanTexture* tex);
		std::unique_ptr<VulkanTexture> create_texture_smart(const TextureDesc& desc);
	};
}
