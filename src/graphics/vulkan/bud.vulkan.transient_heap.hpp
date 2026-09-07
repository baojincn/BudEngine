#pragma once

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

#include "src/graphics/bud.graphics.transient_heap.hpp"
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.pool.hpp"

namespace bud::graphics::vulkan {

	struct VulkanHeapChunk {
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaVirtualBlock virtual_block = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		uint32_t memory_type_index = 0;
	};

	struct CachedPlacedImage {
		TextureHandle handle;
		std::shared_ptr<VulkanTexture> texture;
		uint32_t chunk_index = 0;
		VkDeviceSize offset = 0;
		VkDeviceSize size = 0;
		size_t desc_hash = 0;
		uint32_t last_used_frame = 0;
	};

	class VulkanTransientHeap : public TransientHeapBase {
	public:
		VulkanTransientHeap(VkDevice device, VkPhysicalDevice physical_device, VmaAllocator allocator, VulkanResourcePool* pool);
		~VulkanTransientHeap() override;

		TextureHandle allocate_texture(const TextureDesc& desc, TransientAllocation& out_alloc) override;
		void virtual_free(const TransientAllocation& alloc) override;
		void reset_frame() override;
		TransientHeapStats get_stats() const override;
		void clear_cache() override;

	private:
		VkDevice device = VK_NULL_HANDLE;
		VkPhysicalDevice physical_device = VK_NULL_HANDLE;
		VmaAllocator vma_allocator = VK_NULL_HANDLE;
		VulkanResourcePool* pool = nullptr;

		mutable std::mutex mutex;
		std::vector<VulkanHeapChunk> chunks;
		std::unordered_map<size_t, VkMemoryRequirements> mem_reqs_cache;

		struct CacheKey {
			uint32_t chunk_index = 0;
			VkDeviceSize offset = 0;
			size_t desc_hash = 0;
			bool operator==(const CacheKey& other) const = default;
		};

		struct CacheKeyHash {
			size_t operator()(const CacheKey& k) const noexcept {
				size_t h = std::hash<uint32_t>{}(k.chunk_index);
				h ^= std::hash<uint64_t>{}(k.offset) + 0x9e3779b9 + (h << 6) + (h >> 2);
				h ^= k.desc_hash + 0x9e3779b9 + (h << 6) + (h >> 2);
				return h;
			}
		};

		std::unordered_map<CacheKey, CachedPlacedImage, CacheKeyHash> image_cache;

		size_t current_allocated_bytes = 0;
		size_t peak_allocated_bytes = 0;
		uint32_t current_frame = 0;

		uint32_t allocate_chunk(VkDeviceSize min_size, uint32_t memory_type_bits);
		VkMemoryRequirements get_image_memory_requirements(const TextureDesc& desc, size_t desc_hash);
		size_t hash_desc(const TextureDesc& desc) const;
	};

}
