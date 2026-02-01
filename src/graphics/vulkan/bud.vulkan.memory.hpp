#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <print>
#include <stdexcept>

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.memory.hpp" // For Allocator base if needed, or just types

// Forward decl
namespace bud::graphics { struct MemoryBlock; enum class MemoryUsage; enum class ResourceState; class Allocator; }

namespace bud::graphics::vulkan {

	// 线性分配器页 (用于 Transient 和 Staging)
	struct LinearPage {
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		VkDeviceSize offset = 0; // 当前分配指针
		void* mapped_ptr = nullptr;

		// 尝试从页内分配
		bool try_alloc(VkDeviceSize req_size, VkDeviceSize alignment, VkDeviceSize& out_offset);
		void reset();
	};

	class VulkanMemoryAllocator : public bud::graphics::Allocator {
	public:
		VulkanMemoryAllocator(VkDevice device, VkPhysicalDevice phy_device, uint32_t frames_in_flight);

		void init() override;
		void cleanup() override;
		void on_frame_begin(uint32_t frame_index) override;

		// --- 1. 静态分配 (Fallback 到直接分配) ---
		MemoryBlock alloc_static(uint64_t size, uint64_t alignment, uint32_t memory_type_bits, MemoryUsage usage) override;

		// 释放静态资源
		void free(const MemoryBlock& block) override;

		// --- 2. 帧临时分配 (线性) ---
		MemoryBlock alloc_frame_transient(uint64_t size, uint64_t alignment, uint32_t memory_type_bits) override;

		// --- 3. 上传堆分配 (多帧线性) ---
		MemoryBlock alloc_staging(uint64_t size, uint64_t alignment) override;

	private:
		VkDevice device;
		VkPhysicalDevice phy_device;
		VkPhysicalDeviceMemoryProperties mem_props;
		uint32_t frames_in_flight;
		uint32_t current_frame_index = 0;
		std::mutex mutex;

		LinearPage transient_page; // GPU-Only (Render Targets)
		std::vector<LinearPage> staging_pages; // CPU-to-GPU (Uniforms), Per Frame

		// 通用页创建函数
		void create_page(VkDeviceSize size, VkMemoryPropertyFlags props, LinearPage& out_page);
		uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties);
	};
}
