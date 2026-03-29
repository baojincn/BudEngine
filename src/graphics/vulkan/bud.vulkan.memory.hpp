#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <print>
#include <stdexcept>

#include <vma/vk_mem_alloc.h>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.memory.hpp" // For Allocator base if needed, or just types

// Forward decl
namespace bud::graphics { struct BufferHandle; enum class MemoryUsage; enum class ResourceState; class Allocator; }

namespace bud::graphics::vulkan {

	struct VmaLinearPage {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo alloc_info = {};
		VkDeviceSize size = 0;
		VkDeviceSize offset = 0; // 当前分配指针
		void* mapped_ptr = nullptr;

		bool try_alloc(VkDeviceSize req_size, VkDeviceSize alignment, VkDeviceSize& out_offset);
		void reset();
	};

	struct VulkanBuffer {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo alloc_info = {};
		bool owns_allocation = true;
		void* mapped_ptr = nullptr;
		uint64_t size = 0;
	};

	class VulkanMemoryAllocator : public bud::graphics::Allocator {
	public:
		VulkanMemoryAllocator(VkInstance instance, VkDevice device, VkPhysicalDevice phy_device, uint32_t frames_in_flight);

		void init() override;
		void cleanup() override;
		void on_frame_begin(uint32_t frame_index) override;

		// 1. GPU 专用资源
		bud::graphics::BufferHandle alloc_gpu(uint64_t size, bud::graphics::ResourceState usage) override;

		// 2. 帧临时分配 (线性)
		bud::graphics::BufferHandle alloc_frame_transient(uint64_t size, uint64_t alignment) override;

		// 3. 上传堆分配 (多帧线性)
		bud::graphics::BufferHandle alloc_staging(uint64_t size, uint64_t alignment = 256) override;

		// 4. 持久映射分配
		bud::graphics::BufferHandle alloc_persistent(uint64_t size, bud::graphics::ResourceState usage) override;

		// 5. 纹理分配
		bud::graphics::Texture* create_texture(const bud::graphics::TextureDesc& desc) override;

		VmaAllocator get_vma_allocator() const { return vma_allocator; }

        // Deferred free support: buffers/textures marked for freeing when given frame is reached
        // Take BufferHandle by value to transfer ownership into the deferred queue.
        void defer_free(bud::graphics::BufferHandle handle, uint32_t frame_index) override;
		void defer_free(bud::graphics::Texture* texture, uint32_t frame_index) override;

	private:
		VkInstance instance;
		VkDevice device;
		VkPhysicalDevice phy_device;
		VmaAllocator vma_allocator = VK_NULL_HANDLE;
		uint32_t frames_in_flight;
		uint32_t current_frame_index = 0;
		std::mutex mutex;

		std::vector<VmaLinearPage> staging_pages; // CPU-to-GPU (Uniforms), Per Frame
		std::vector<std::vector<bud::graphics::BufferHandle>> deferred_free_buffers;
		std::vector<std::vector<bud::graphics::Texture*>> deferred_free_textures;
	};
}
