module;
#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <print>
#include <stdexcept>

export module bud.vulkan.memory;

import bud.graphics.memory;
import bud.graphics.types;

namespace bud::graphics::vulkan {

	// 线性分配器页 (用于 Transient 和 Staging)
	struct LinearPage {
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		VkDeviceSize offset = 0; // 当前分配指针
		void* mapped_ptr = nullptr;

		// 尝试从页内分配
		bool try_alloc(VkDeviceSize req_size, VkDeviceSize alignment, VkDeviceSize& out_offset) {
			// 计算对齐
			VkDeviceSize aligned_offset = (offset + alignment - 1) & ~(alignment - 1);
			if (aligned_offset + req_size > size) return false;

			out_offset = aligned_offset;
			offset = aligned_offset + req_size;
			return true;
		}

		void reset() { offset = 0; }
	};

	export class VulkanMemoryAllocator : public bud::graphics::Allocator {
	public:
		VulkanMemoryAllocator(VkDevice device, VkPhysicalDevice phy_device, uint32_t frames_in_flight)
			: m_device(device), m_phy_device(phy_device), m_frames_in_flight(frames_in_flight) {
		}

		void init() override {
			vkGetPhysicalDeviceMemoryProperties(m_phy_device, &m_mem_props);

			// 1. 初始化 Transient Pool (RenderGraph 显存池)
			// 预分配 256MB DeviceLocal 显存，专门给 RT 使用
			// 注意：RT 通常很大，256MB 可能只够 1080p 的几个 G-Buffer，正式引擎通常是 512MB+ 或动态扩容
			create_page(256 * 1024 * 1024, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_transient_page);
			std::println("[Memory] Transient Heap Initialized: 256 MB");

			// 2. 初始化 Staging Pools (CPU上传池)
			// 为每一帧创建一个独立的上传堆 (64MB x FramesInFlight)
			m_staging_pages.resize(m_frames_in_flight);
			for (uint32_t i = 0; i < m_frames_in_flight; ++i) {
				create_page(64 * 1024 * 1024, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m_staging_pages[i]);
			}
			std::println("[Memory] Staging Heaps Initialized: 64 MB x {}", m_frames_in_flight);
		}

		void cleanup() override {
			// 释放 Transient
			if (m_transient_page.memory) vkFreeMemory(m_device, m_transient_page.memory, nullptr);

			// 释放 Staging
			for (auto& page : m_staging_pages) {
				if (page.memory) vkFreeMemory(m_device, page.memory, nullptr);
			}
		}

		// 帧开始时调用，更新当前帧索引
		void on_frame_begin(uint32_t frame_index) override {
			std::lock_guard lock(m_mutex);
			m_current_frame_index = frame_index;

			// 重置 Transient (因为是帧临时资源，上一帧肯定用完了，或者有 Barrier 保护)
			m_transient_page.reset();

			// 重置当前帧的 Staging Page (因为有 Fence 保护，说明 GPU 已经处理完这一帧之前的指令了)
			if (frame_index < m_staging_pages.size()) {
				m_staging_pages[frame_index].reset();
			}
		}

		// --- 1. 静态分配 (Fallback 到直接分配) ---
		// 长期存在的资源 (Texture, VertexBuffer) 使用此方法
		MemoryBlock alloc_static(uint64_t size, uint64_t alignment, uint32_t memory_type_bits, MemoryUsage usage) override {
			VkMemoryAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = size;

			VkMemoryPropertyFlags props = 0;
			if (usage == MemoryUsage::GpuOnly) props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			else props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

			alloc_info.memoryTypeIndex = find_memory_type(memory_type_bits, props);

			VkDeviceMemory mem;
			if (vkAllocateMemory(m_device, &alloc_info, nullptr, &mem) != VK_SUCCESS) {
				throw std::runtime_error("Failed to allocate static memory!");
			}

			MemoryBlock block;
			block.internal_handle = mem;
			block.size = size;
			block.offset = 0;

			if (usage != MemoryUsage::GpuOnly) {
				vkMapMemory(m_device, mem, 0, size, 0, &block.mapped_ptr);
			}

			return block;
		}

		// 释放静态资源
		void free(const MemoryBlock& block) override {
			// 只有 alloc_static 分配的独立内存才需要 free
			// 判断是否属于我们的 Pool (简单的指针比较)
			bool is_transient = (block.internal_handle == m_transient_page.memory);
			bool is_staging = false;
			for (const auto& page : m_staging_pages) {
				if (block.internal_handle == page.memory) { is_staging = true; break; }
			}

			if (!is_transient && !is_staging && block.internal_handle) {
				vkFreeMemory(m_device, static_cast<VkDeviceMemory>(block.internal_handle), nullptr);
			}
		}

		// --- 2. 帧临时分配 (线性) ---
		// RenderGraph 的 RenderTargets 使用此方法
		MemoryBlock alloc_frame_transient(uint64_t size, uint64_t alignment, uint32_t memory_type_bits) override {
			VkDeviceSize offset = 0;
			bool success = false;

			{
				std::lock_guard lock(m_mutex);
				// 假设 memory_type_bits 兼容 DeviceLocal
				success = m_transient_page.try_alloc(size, alignment, offset);
			}

			if (!success) {
				// 显存池满了！降级为直接分配 (会有性能警告，但不会崩)
				// std::println(stderr, "[Memory] Warning: Transient Heap Overflow! Fallback to static alloc.");
				return alloc_static(size, alignment, memory_type_bits, MemoryUsage::GpuOnly);
			}

			MemoryBlock block;
			block.internal_handle = m_transient_page.memory;
			block.offset = offset;
			block.size = size;
			block.mapped_ptr = nullptr; // GPU Only
			return block;
		}

		// --- 3. 上传堆分配 (多帧线性) ---
		// Uniform Buffer, Dynamic VB/IB 使用此方法
		MemoryBlock alloc_staging(uint64_t size, uint64_t alignment) override {
			VkDeviceSize offset = 0;
			bool success = false;
			LinearPage* current_page = nullptr;

			{
				std::lock_guard lock(m_mutex);
				if (m_current_frame_index < m_staging_pages.size()) {
					current_page = &m_staging_pages[m_current_frame_index];
					success = current_page->try_alloc(size, alignment, offset);
				}
			}

			if (!success || !current_page) {
				// 这一帧的上传堆满了，降级为直接分配
				return alloc_static(size, alignment, 0xFFFFFFFF, MemoryUsage::CpuToGpu);
			}

			MemoryBlock block;
			block.internal_handle = current_page->memory;
			block.offset = offset;
			block.size = size;
			// 计算 CPU 写入指针
			block.mapped_ptr = static_cast<uint8_t*>(current_page->mapped_ptr) + offset;
			return block;
		}

	private:
		VkDevice m_device;
		VkPhysicalDevice m_phy_device;
		VkPhysicalDeviceMemoryProperties m_mem_props;
		uint32_t m_frames_in_flight;
		uint32_t m_current_frame_index = 0;
		std::mutex m_mutex;

		LinearPage m_transient_page; // GPU-Only (Render Targets)
		std::vector<LinearPage> m_staging_pages; // CPU-to-GPU (Uniforms), Per Frame

		// 通用页创建函数
		void create_page(VkDeviceSize size, VkMemoryPropertyFlags props, LinearPage& out_page) {
			VkMemoryAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			alloc_info.allocationSize = size;
			// 寻找最合适的 Memory Type (通常选第一个符合要求的)
			alloc_info.memoryTypeIndex = find_memory_type(0xFFFFFFFF, props);

			if (vkAllocateMemory(m_device, &alloc_info, nullptr, &out_page.memory) == VK_SUCCESS) {
				out_page.size = size;
				out_page.offset = 0;

				// 如果是 Host Visible，立刻 Map 出来备用
				if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
					vkMapMemory(m_device, out_page.memory, 0, size, 0, &out_page.mapped_ptr);
				}
			}
			else {
				throw std::runtime_error("Failed to allocate memory page!");
			}
		}

		uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) {
			for (uint32_t i = 0; i < m_mem_props.memoryTypeCount; i++) {
				if ((type_filter & (1 << i)) && (m_mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
					return i;
				}
			}
			throw std::runtime_error("Failed to find suitable memory type!");
		}
	};
}
