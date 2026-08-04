#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <print>
#include <stdexcept>
#include <unordered_set>

#include <vma/vk_mem_alloc.h>
#include <source_location>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.memory.hpp"

namespace bud::graphics {
	struct BufferHandle;
	enum class MemoryUsage;
	enum class ResourceState;
	class Allocator;
}

namespace bud::graphics::vulkan {

	struct VmaLinearPage {
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo alloc_info = {};
		VkDeviceSize size = 0;
		VkDeviceSize offset = 0;
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

		// Optional back-reference to allocator for RAII cleanup.
		VmaAllocator allocator = VK_NULL_HANDLE;
		// Optional pointer to owning VulkanMemoryAllocator used only for unregistering
		// when tracking is enabled. May be null when tracking is disabled.
		class VulkanMemoryAllocator* owning_allocator = nullptr;

		// RAII destructor: will unregister and destroy underlying VMA allocation
		// if this wrapper owns the allocation. Implementation in cpp.
		~VulkanBuffer();
	};

    class VulkanMemoryAllocator : public bud::graphics::Allocator {
    public:
        VulkanMemoryAllocator(VkInstance instance, VkDevice device, VkPhysicalDevice phy_device, uint32_t frames_in_flight, uint32_t api_version = VK_API_VERSION_1_1);

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

		// 5. 回读分配
		bud::graphics::BufferHandle alloc_read_back(uint64_t size) override;

		// 6. 纹理分配
		bud::graphics::Texture* create_texture(const bud::graphics::TextureDesc& desc) override;

        VmaAllocator get_vma_allocator() const { return vma_allocator; }

        // Debug tracking to detect outstanding allocations at cleanup.
        // Use explicit registration for buffer vs image wrappers so cleanup can
        // correctly destroy remaining VMA allocations without relying on unsafe casts.
        // Tracking API: enabled when BUD_VMA_TRACKING is defined. When disabled,
        // inline no-op implementations are provided so callers do not need to
        // conditionalize their calls.
        void register_allocation_buffer(void* buffer_wrapper, const std::source_location& loc = std::source_location::current());
        void unregister_allocation_buffer(void* buffer_wrapper, const std::source_location& loc = std::source_location::current());
        void register_allocation_image(void* image_wrapper, const std::source_location& loc = std::source_location::current());
        void unregister_allocation_image(void* image_wrapper, const std::source_location& loc = std::source_location::current());
        // Return number of live buffer wrappers currently tracked
        // (diagnostic helper - may be removed)
        size_t get_live_buffer_wrapper_count();

        // Deferred free support: buffers/textures marked for freeing when given frame is reached
        // Take BufferHandle by value to transfer ownership into the deferred queue.
        void defer_free(bud::graphics::BufferHandle handle, uint32_t frame_index) override;
		void defer_free(bud::graphics::Texture* texture, uint32_t frame_index) override;

	private:
		VkInstance instance;
		VkDevice device;
		VkPhysicalDevice phy_device;
        uint32_t vulkan_api_version = VK_API_VERSION_1_1;
		VmaAllocator vma_allocator = VK_NULL_HANDLE;
		uint32_t frames_in_flight;
		uint32_t current_frame_index = 0;
		std::mutex mutex;


		std::vector<VmaLinearPage> staging_pages; // CPU-to-GPU (Uniforms), Per Frame
		std::vector<std::vector<bud::graphics::BufferHandle>> deferred_free_buffers;
		std::vector<std::vector<bud::graphics::Texture*>> deferred_free_textures;

        // Track live VMA allocations to allow forced cleanup on shutdown
        // Separate sets for buffers and images to avoid unsafe casting.
        std::unordered_set<void*> live_buffer_wrappers;
        std::unordered_set<void*> live_image_wrappers;
        // Optional diagnostic mapping from wrapper pointer to allocation origin
        std::unordered_map<void*, std::string> live_buffer_origins;
	};
}
