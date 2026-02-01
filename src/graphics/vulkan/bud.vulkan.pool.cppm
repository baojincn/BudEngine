module;
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>  // for unique_ptr
#include <utility> // for move

export module bud.vulkan.pool;

import bud.graphics.pool;
import bud.graphics.types;
import bud.vulkan.types;
import bud.vulkan.memory;

export namespace bud::graphics::vulkan {

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
