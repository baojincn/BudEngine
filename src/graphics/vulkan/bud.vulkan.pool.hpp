#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>  // for unique_ptr
#include <utility> // for move
#include <mutex>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.pool.hpp"
#include "src/graphics/bud.graphics.rhi.hpp" // For ResourcePool base
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"

namespace bud::graphics::vulkan {

	struct VulkanTextureSlot {
		std::shared_ptr<VulkanTexture> texture;
		bool in_use = false;
	};

	struct VulkanBufferSlot {
		std::shared_ptr<VulkanBuffer> buffer;
		bool in_use = false;
	};

	class VulkanResourcePool : public ResourcePool {
	public:
		VulkanResourcePool(VkDevice device, VulkanMemoryAllocator* allocator);
		~VulkanResourcePool();

		void cleanup();

		// Texture interface
		TextureHandle acquire_texture(const TextureDesc& desc) override;
		void release_texture(TextureHandle handle) override;
		Texture* get_texture(TextureHandle handle) override;
		const Texture* get_texture(TextureHandle handle) const override;
		TextureDesc get_texture_desc(TextureHandle handle) const override;
		TextureHandle register_texture(std::shared_ptr<VulkanTexture> tex);
		void unregister_texture(TextureHandle handle);

		// Buffer interface
		BufferHandle acquire_buffer(const BufferDesc& desc) override;
		void release_buffer(BufferHandle handle) override;
		Buffer* get_buffer(BufferHandle handle) override;
		const Buffer* get_buffer(BufferHandle handle) const override;
		BufferDesc get_buffer_desc(BufferHandle handle) const override;
		BufferHandle register_buffer(std::shared_ptr<VulkanBuffer> buf);

		void tick() override;

	private:
		VkDevice device;
		VulkanMemoryAllocator* allocator;
		mutable std::mutex mutex;

		std::vector<VulkanTextureSlot> texture_slots;
		std::vector<uint32_t> free_texture_indices;

		using TexturePtr = std::shared_ptr<VulkanTexture>;
		std::unordered_map<size_t, std::vector<TexturePtr>> image_pool;

		std::vector<VulkanBufferSlot> buffer_slots;
		std::vector<uint32_t> free_buffer_indices;

		using BufferPtr = std::shared_ptr<VulkanBuffer>;
		std::unordered_map<size_t, std::vector<BufferPtr>> buffer_pool;

		size_t hash_desc(const TextureDesc& desc);
		size_t hash_desc(const BufferDesc& desc);
		void destroy_vulkan_objects(VulkanTexture* tex);
		void destroy_vulkan_objects(VulkanBuffer* buf);
		std::shared_ptr<VulkanTexture> create_texture_smart(const TextureDesc& desc);
		std::shared_ptr<VulkanBuffer> create_buffer_smart(const BufferDesc& desc);
	};
}
