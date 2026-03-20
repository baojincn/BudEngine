#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <print>
#include <stdexcept>

#define VMA_IMPLEMENTATION
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics::vulkan {

    // LinearPage

    bool VmaLinearPage::try_alloc(VkDeviceSize req_size, VkDeviceSize alignment, VkDeviceSize& out_offset) {
        VkDeviceSize aligned_offset = (offset + alignment - 1) & ~(alignment - 1);
        if (aligned_offset + req_size > size) return false;

        out_offset = aligned_offset;
        offset = aligned_offset + req_size;
        return true;
    }

    void VmaLinearPage::reset() { offset = 0; }

    // VulkanMemoryAllocator

    VulkanMemoryAllocator::VulkanMemoryAllocator(VkInstance instance, VkDevice device, VkPhysicalDevice phy_device, uint32_t frames_in_flight)
        : instance(instance), device(device), phy_device(phy_device), frames_in_flight(frames_in_flight) {
    }

    void VulkanMemoryAllocator::init() {
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        allocatorInfo.physicalDevice = phy_device;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        // allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT; // Enable if needed later

        if (vmaCreateAllocator(&allocatorInfo, &vma_allocator) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create VMA Allocator!");
        }

        // Initialize staging pages. For VMA, Transient pages can just use VmaAllocator directly,
        // but we'll keep the staging pages as large mapped Vulkan buffers for fast CPU writes.
        staging_pages.resize(frames_in_flight);
        for (uint32_t i = 0; i < frames_in_flight; ++i) {
            VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = 64 * 1024 * 1024; // 64 MB
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo = {};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo vmaInfo;
            // Hack: store VMA handles in the LinearPage's raw pointers for now 
            // since LinearPage was designed for raw memory.
            VkBuffer buf;
            VmaAllocation alloc;
            vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo, &buf, &alloc, &vmaInfo);
            
            // We use LinearPage to manage offsets within this giant VMA buffer
            staging_pages[i].buffer = buf;
            staging_pages[i].allocation = alloc; // Store VmaAllocation here
            staging_pages[i].size = bufferInfo.size;
            staging_pages[i].mapped_ptr = vmaInfo.pMappedData;
        }
        bud::print("[Memory] VMA Staging Heaps Initialized: 64 MB x {}", frames_in_flight);
    }

    void VulkanMemoryAllocator::cleanup() {
        for (auto& page : staging_pages) {
            if (page.buffer != VK_NULL_HANDLE && vma_allocator != VK_NULL_HANDLE) {
                vmaDestroyBuffer(vma_allocator, page.buffer, page.allocation);
                page.buffer = VK_NULL_HANDLE;
            }
        }
        if (vma_allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(vma_allocator);
            vma_allocator = VK_NULL_HANDLE;
        }
    }

    void VulkanMemoryAllocator::on_frame_begin(uint32_t frame_index) {
        std::lock_guard lock(mutex);
        current_frame_index = frame_index;

        if (frame_index < staging_pages.size()) {
            staging_pages[frame_index].reset();
        }
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_frame_transient(uint64_t size, uint64_t alignment) {
        // Obsolete, transient resources are now created directly via VMA in RHI
        return {};
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_staging(uint64_t size, uint64_t alignment) {
        VkDeviceSize offset = 0;
        bool success = false;
        VmaLinearPage* current_page = nullptr;

        {
            std::lock_guard lock(mutex);
            if (current_frame_index < staging_pages.size()) {
                current_page = &staging_pages[current_frame_index];
                success = current_page->try_alloc(size, alignment, offset);
            }
        }

        if (!success || !current_page) {
            // fallback if staging page is full (rare in our 64MB setup)
            throw std::runtime_error("Staging page overflowed! No fallback implemented yet.");
        }

        bud::graphics::BufferHandle block;
        // Allocate a new VulkanBuffer wrapper
        auto* vk_buf = new VulkanBuffer();
        vk_buf->buffer = current_page->buffer;
        vk_buf->allocation = current_page->allocation;
        vk_buf->owns_allocation = false; // It's a sub-allocation, so it shouldn't destroy the parent buffer

        block.internal_state = vk_buf;
        block.offset = offset;
        block.size = size;
        block.mapped_ptr = static_cast<uint8_t*>(current_page->mapped_ptr) + offset;
        return block;
    }

}
