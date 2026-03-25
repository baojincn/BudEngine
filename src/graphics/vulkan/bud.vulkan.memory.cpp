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
        if (aligned_offset + req_size > size)
            return false;

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
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_1;
        allocatorInfo.physicalDevice = phy_device;
        allocatorInfo.device = device;
        allocatorInfo.instance = instance;
        // allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        if (vmaCreateAllocator(&allocatorInfo, &vma_allocator) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create VMA Allocator!");
        }

        // Initialize staging pages. For VMA, Transient pages can just use VmaAllocator directly,
        // but we'll keep the staging pages as large mapped Vulkan buffers for fast CPU writes.
        staging_pages.resize(frames_in_flight);
        deferred_free_buffers.resize(frames_in_flight);
        deferred_free_textures.resize(frames_in_flight);
        for (uint32_t i = 0; i < frames_in_flight; ++i) {
            VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = 64 * 1024 * 1024; // 64 MB
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT; // Allow binding as vertex/index buffer

            VmaAllocationCreateInfo allocInfo = {};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo vmaInfo;
            VkBuffer buf;
            VmaAllocation alloc;
            VkResult r = vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo, &buf, &alloc, &vmaInfo);
            if (r != VK_SUCCESS) {
                // Cleanup any previously created pages and destroy allocator
                for (uint32_t j = 0; j < i; ++j) {
                    if (staging_pages[j].buffer != VK_NULL_HANDLE && staging_pages[j].allocation != VK_NULL_HANDLE && vma_allocator) {
                        vmaDestroyBuffer(vma_allocator, staging_pages[j].buffer, staging_pages[j].allocation);
                        staging_pages[j].buffer = VK_NULL_HANDLE;
                        staging_pages[j].allocation = VK_NULL_HANDLE;
                        staging_pages[j].mapped_ptr = nullptr;
                        staging_pages[j].size = 0;
                    }
                }
                if (vma_allocator) {
                    vmaDestroyAllocator(vma_allocator);
                    vma_allocator = nullptr;
                }
                throw std::runtime_error("Failed to create VMA staging buffer for frame " + std::to_string(i));
            }

            staging_pages[i].buffer = buf;
            staging_pages[i].allocation = alloc;
            staging_pages[i].size = bufferInfo.size;
            staging_pages[i].mapped_ptr = vmaInfo.pMappedData;
        }
        bud::print("[Memory] VMA Staging Heaps Initialized: 64 MB x {}", frames_in_flight);
    }

    void VulkanMemoryAllocator::cleanup() {
        // Flush all deferred frees before destroying allocator
        for (uint32_t i = 0; i < frames_in_flight; ++i) {
            on_frame_begin(i);
        }
        // Destroy staging pages first (they depend on the VMA allocator)
        if (vma_allocator) {
            for (auto& page : staging_pages) {
                if (page.buffer != VK_NULL_HANDLE && page.allocation != VK_NULL_HANDLE) {
                    vmaDestroyBuffer(vma_allocator, page.buffer, page.allocation);
                    page.buffer = VK_NULL_HANDLE;
                    page.allocation = VK_NULL_HANDLE;
                    page.mapped_ptr = nullptr;
                    page.size = 0;
                }
            }
        }

        if (vma_allocator) {
            vmaDestroyAllocator(vma_allocator);
            vma_allocator = nullptr;
        }
    }

    void VulkanMemoryAllocator::on_frame_begin(uint32_t frame_index) {
        std::lock_guard lock(mutex);
        // bud::print("[VulkanMemoryAllocator::on_frame_begin] frame_index={}", frame_index); // [LOG REDUCED]
        current_frame_index = frame_index;

        // Free deferred buffers/textures scheduled for this frame (safe point: RHI waits on fence before calling on_frame_begin)
        if (frame_index < deferred_free_buffers.size()) {
            for (auto& handle : deferred_free_buffers[frame_index]) {
                if (!handle.is_valid()) continue;
                auto* vk_buf = static_cast<VulkanBuffer*>(handle.internal_state);
                if (!vk_buf) continue;
                if (vk_buf->owns_allocation) {
                    if (vma_allocator && vk_buf->buffer != VK_NULL_HANDLE) {
                        vmaDestroyBuffer(vma_allocator, vk_buf->buffer, vk_buf->allocation);
                    }
                }
                delete vk_buf;
            }
            deferred_free_buffers[frame_index].clear();
        }

        // Textures deferred: allocator doesn't own higher-level texture lifecycle; clear list for now
        if (frame_index < deferred_free_textures.size()) {
            deferred_free_textures[frame_index].clear();
        }

        if (frame_index < staging_pages.size()) {
            staging_pages[frame_index].reset();
        }
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_frame_transient(uint64_t size, uint64_t alignment) {
        // Obsolete, transient resources are now created directly via VMA in RHI
        return {};
    }

    void VulkanMemoryAllocator::defer_free(const bud::graphics::BufferHandle& handle, uint32_t frame_index) {
        if (!handle.is_valid())
            return;
        std::lock_guard lock(mutex);
        if (deferred_free_buffers.empty())
            return;
        uint32_t idx = frame_index % deferred_free_buffers.size();
        deferred_free_buffers[idx].push_back(handle);
    }

    void VulkanMemoryAllocator::defer_free(bud::graphics::Texture* texture, uint32_t frame_index) {
        if (!texture)
            return;
        std::lock_guard lock(mutex);
        if (deferred_free_textures.empty())
            return;
        uint32_t idx = frame_index % deferred_free_textures.size();
        deferred_free_textures[idx].push_back(texture);
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

        bud::graphics::BufferHandle block;

        if (success && current_page) {
            // Sub-allocation from a per-frame staging page
            auto* vk_buf = new VulkanBuffer();
            vk_buf->buffer = current_page->buffer;
            vk_buf->allocation = current_page->allocation;
            vk_buf->owns_allocation = false; // It's a sub-allocation, so it shouldn't destroy the parent buffer

            block.internal_state = vk_buf;
            block.offset = offset;
            block.size = size;
            block.mapped_ptr = static_cast<uint8_t*>(current_page->mapped_ptr) + offset;
            // bud::print("[VulkanMemoryAllocator][alloc_staging] Suballoc VkBuffer={} size={} usage=STAGING_PAGE", (void*)vk_buf->buffer, size); // [LOG REDUCED]
            return block;
        }

        // Fallback: allocate a dedicated temporary mapped buffer for this staging request
        // This avoids throwing in runtime when the ring buffer is exhausted.
        VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size = size;
        // Always allow binding as vertex and index buffer for UI/upload buffers
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        auto* vk_buf = new VulkanBuffer();
        VmaAllocationInfo alloc_result_info;
        VkResult r = vmaCreateBuffer(vma_allocator, &buffer_info, &alloc_info, &vk_buf->buffer, &vk_buf->allocation, &alloc_result_info);
        if (r != VK_SUCCESS) {
            delete vk_buf;
            // As a last resort, return empty handle to let caller handle gracefully
            std::string err = std::format("alloc_staging fallback vmaCreateBuffer failed: {}", (int)r);
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return {};
#endif
        }

        vk_buf->mapped_ptr = alloc_result_info.pMappedData;
        vk_buf->size = size;
        vk_buf->owns_allocation = true;

        block.internal_state = vk_buf;
        block.offset = 0;
        block.size = size;
        block.mapped_ptr = vk_buf->mapped_ptr;
        // bud::print("[VulkanMemoryAllocator][alloc_staging] VkBuffer={} size={} usage=TRANSFER_SRC|VERTEX|INDEX", (void*)vk_buf->buffer, size); // [LOG REDUCED]
        return block;
    }

    static VkBufferUsageFlags get_vk_buffer_usage(bud::graphics::ResourceState usage_state) {
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; // Default to allow uploads
        if (usage_state == bud::graphics::ResourceState::VertexBuffer) {
            usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        } else if (usage_state == bud::graphics::ResourceState::IndexBuffer) {
            usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        } else if (usage_state == bud::graphics::ResourceState::IndirectArgument) {
            usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        } else if (usage_state == bud::graphics::ResourceState::UnorderedAccess) {
            usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        } else if (usage_state == bud::graphics::ResourceState::ShaderResource) {
            usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        return usage;
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_gpu(uint64_t size, bud::graphics::ResourceState usage) {
        VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size = size;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_info.usage = get_vk_buffer_usage(usage);

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; // Strictly Device Local

        // For UAV/TransferSrc buffers (like stats counter), ensure host access so we can map it for readback.
        if (usage == bud::graphics::ResourceState::UnorderedAccess || (buffer_info.usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            alloc_info.requiredFlags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }

        auto* vk_buf = new VulkanBuffer();
        VmaAllocationInfo alloc_result_info;
        if (vmaCreateBuffer(vma_allocator, &buffer_info, &alloc_info, &vk_buf->buffer, &vk_buf->allocation, &alloc_result_info) != VK_SUCCESS) {
            delete vk_buf;
            return {};
        }

        vk_buf->mapped_ptr = alloc_result_info.pMappedData;
        vk_buf->size = size;
        vk_buf->owns_allocation = true;

        bud::print("[VulkanMemoryAllocator][alloc_gpu] VkBuffer={} size={} usage={} (ResourceState={})", (void*)vk_buf->buffer, size, (uint32_t)buffer_info.usage, (int)usage);

        bud::graphics::BufferHandle handle;
        handle.internal_state = vk_buf;
        handle.size = size;
        handle.mapped_ptr = vk_buf->mapped_ptr;
        return handle;
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_persistent(uint64_t size, bud::graphics::ResourceState usage) {
        VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size = size;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_info.usage = get_vk_buffer_usage(usage);
        buffer_info.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // usually CPU writes to this, then it might be transferred or used directly

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        
        auto* vk_buf = new VulkanBuffer();
        VmaAllocationInfo alloc_result_info;
        if (vmaCreateBuffer(vma_allocator, &buffer_info, &alloc_info, &vk_buf->buffer, &vk_buf->allocation, &alloc_result_info) != VK_SUCCESS) {
            delete vk_buf;
            return {};
        }

        vk_buf->mapped_ptr = alloc_result_info.pMappedData;
        vk_buf->size = size;
        vk_buf->owns_allocation = true;

        bud::print("[VulkanMemoryAllocator][alloc_persistent] VkBuffer={} size={} usage={} (ResourceState={})", (void*)vk_buf->buffer, size, (uint32_t)buffer_info.usage, (int)usage);

        bud::graphics::BufferHandle handle;
        handle.internal_state = vk_buf;
        handle.size = size;
        handle.mapped_ptr = vk_buf->mapped_ptr;
        return handle;
    }

    bud::graphics::Texture* VulkanMemoryAllocator::create_texture(const bud::graphics::TextureDesc& desc) {
        return nullptr; // Stub for Phase 1
    }
}
