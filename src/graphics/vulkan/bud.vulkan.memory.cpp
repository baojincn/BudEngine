#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <string>
#include <print>
#include <stdexcept>
#include "src/threading/bud.threading.hpp"

#define VMA_IMPLEMENTATION
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/bud.graphics.types.hpp"

// Temporary safety: ensure tracking macro is disabled by default to avoid
// changing shutdown ordering while we iterate on diagnostics. If you need
// tracking, define BUD_VMA_TRACKING explicitly when building and re-run
// tests.
#undef BUD_VMA_TRACKING

namespace bud::graphics::vulkan {

    // live allocation tracking (only used if register/unregister are called)
    // stored inside allocator instance as member (defined below)

    // LinearPage

    bool VmaLinearPage::try_alloc(VkDeviceSize req_size, VkDeviceSize alignment, VkDeviceSize& out_offset) {
        VkDeviceSize aligned_offset = (offset + alignment - 1) & ~(alignment - 1);
        if (aligned_offset + req_size > size)
            return false;

        out_offset = aligned_offset;
        offset = aligned_offset + req_size;
        return true;
    }

    VulkanBuffer::~VulkanBuffer() {
        // RAII cleanup: unregister if an owning allocator exists (tracking)
#ifdef BUD_VMA_TRACKING
        if (owning_allocator) {
            try { owning_allocator->unregister_allocation_buffer(this); } catch (...) { }
        }
#endif
        if (owns_allocation && allocator) {
            // Safe-guard: try-destroy VMA resource if still present
            if (buffer != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator, buffer, allocation);
            }
        }
    }

    size_t VulkanMemoryAllocator::get_live_buffer_wrapper_count() {
        std::lock_guard lock(mutex);
        return live_buffer_wrappers.size();
    }

    void VmaLinearPage::reset() { offset = 0; }

    // VulkanMemoryAllocator

    VulkanMemoryAllocator::VulkanMemoryAllocator(VkInstance instance, VkDevice device, VkPhysicalDevice phy_device, uint32_t frames_in_flight)
        : instance(instance), device(device), phy_device(phy_device), frames_in_flight(frames_in_flight) {
        
    }

    // (No global debug allocation tracking)

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
        // No allocation tracking initialization required
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
            // Track staging page allocation wrapper (we store the allocation in staging_pages[j].allocation)
            // For staging pages we will ensure they are destroyed explicitly in cleanup using staging_pages entries.

            staging_pages[i].buffer = buf;
            staging_pages[i].allocation = alloc;
            staging_pages[i].size = bufferInfo.size;
            staging_pages[i].mapped_ptr = vmaInfo.pMappedData;
        }
#ifdef BUD_VMA_TRACKING
        bud::print("[Memory] VMA Staging Heaps Initialized: 64 MB x {}", frames_in_flight);
#endif
    }



    void VulkanMemoryAllocator::register_allocation_buffer(void* buffer_wrapper, const std::source_location& loc) {
        if (!buffer_wrapper) return;
#ifdef BUD_VMA_TRACKING
        std::lock_guard lock(mutex);
        live_buffer_wrappers.insert(buffer_wrapper);
        // Record origin for diagnostics
        // Build simple origin string: file:line::function
        try {
            std::string origin = std::string(loc.file_name()) + ":" + std::to_string(loc.line()) + "::" + loc.function_name();
            live_buffer_origins[buffer_wrapper] = origin;
            bud::print("[VulkanMemoryAllocator] register buffer wrapper: ptr={} origin={}", buffer_wrapper, origin);
        } catch (...) {
            // ignore errors
        }
#else
        (void)loc;
#endif
    }

    void VulkanMemoryAllocator::unregister_allocation_buffer(void* buffer_wrapper, const std::source_location& loc) {
        if (!buffer_wrapper) return;
#ifdef BUD_VMA_TRACKING
        std::lock_guard lock(mutex);
        bud::print("[VulkanMemoryAllocator] unregister_allocation_buffer called for ptr={} origin={} (thread={})",
                   buffer_wrapper,
                   (live_buffer_origins.count(buffer_wrapper) ? live_buffer_origins[buffer_wrapper] : std::string("<unknown>")),
                   bud::threading::current_worker_index());
        live_buffer_wrappers.erase(buffer_wrapper);
        live_buffer_origins.erase(buffer_wrapper);
        (void)loc;
#else
        (void)loc;
#endif
    }

    void VulkanMemoryAllocator::register_allocation_image(void* image_wrapper, const std::source_location& loc) {
        if (!image_wrapper) return;
#ifdef BUD_VMA_TRACKING
        std::lock_guard lock(mutex);
        live_image_wrappers.insert(image_wrapper);
        (void)loc;
#else
        (void)loc;
#endif
    }

    void VulkanMemoryAllocator::unregister_allocation_image(void* image_wrapper, const std::source_location& loc) {
        if (!image_wrapper) return;
#ifdef BUD_VMA_TRACKING
        std::lock_guard lock(mutex);
        live_image_wrappers.erase(image_wrapper);
        (void)loc;
#else
        (void)loc;
#endif
    }

    // Allocation tracking removed: register_allocation/unregister_allocation were debug helpers

    void VulkanMemoryAllocator::cleanup() {
        // Flush all deferred frees before destroying allocator
        for (uint32_t i = 0; i < frames_in_flight; ++i) {
            on_frame_begin(i);
        }
        // As an extra safety, move any remaining deferred handles out under lock
        // and release their owners outside the mutex to avoid deadlocks where
        // owner deleters call back into this allocator and try to lock the same mutex.
        std::vector<std::vector<bud::graphics::BufferHandle>> pending_buffers;
        std::vector<std::vector<bud::graphics::Texture*>> pending_textures;
        {
            std::lock_guard lock(mutex);
            pending_buffers = std::move(deferred_free_buffers);
            deferred_free_buffers.clear();
            pending_textures = std::move(deferred_free_textures);
            deferred_free_textures.clear();
        }
        // Release owners outside lock to avoid reentrancy into allocator mutex.
        for (auto &vec : pending_buffers) {
            for (auto &handle : vec) {
                if (handle.owner) {
                    handle.owner.reset();
                }
                handle.internal_state = nullptr;
            }
            vec.clear();
        }
        // Textures are raw pointers; just clear pending list.
        pending_textures.clear();
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
        // Debug: indicate allocator destruction. Only print when tracking is enabled to avoid
        // introducing shutdown-time ordering changes or noisy logs in normal runs.
#ifdef BUD_VMA_TRACKING
        if (vma_allocator) {
            bud::print("[VulkanMemoryAllocator] Destroying VMA allocator (debug). Make sure no outstanding allocations remain.");
        }
#endif

        // Diagnostic reporting and optional forced cleanup when tracking is enabled.
        if (vma_allocator) {
#ifdef BUD_VMA_TRACKING
            // Print diagnostic counts for deferred lists to help identify leaks
            {
                std::lock_guard lock(mutex);
                size_t deferred_buffers = 0;
                for (auto &vec : deferred_free_buffers) deferred_buffers += vec.size();
                size_t deferred_textures = 0;
                for (auto &vec : deferred_free_textures) deferred_textures += vec.size();
                bud::print("[VulkanMemoryAllocator] Diagnostics: deferred_free_buffers_total={}, deferred_free_textures_total={}, live_buffer_wrappers={}, live_image_wrappers={}",
                    deferred_buffers, deferred_textures, live_buffer_wrappers.size(), live_image_wrappers.size());
            }

            // Try to obtain full VMA statistics string to show outstanding allocations.
            // This helps diagnosing which allocations remain when vmaDestroyAllocator triggers an assert.
            char* vma_stats_str = nullptr;
            // Build stats string (detailed map) if supported by VMA build.
            // vmaBuildStatsString returns a string that must be freed with vmaFreeStatsString.
            vmaBuildStatsString(vma_allocator, &vma_stats_str, VK_TRUE);
            if (vma_stats_str) {
                bud::eprint("[VulkanMemoryAllocator] VMA stats before destroy:\n{}", vma_stats_str);
                vmaFreeStatsString(vma_allocator, vma_stats_str);
            }

            // If any live wrappers remain, move them out under lock and destroy
            // their VMA resources outside the mutex to avoid deadlocks with
            // wrapper deleters that may also lock the same mutex.
            std::vector<void*> buffers_to_destroy;
            std::vector<void*> images_to_destroy;
            struct LiveBufDiag { void* ptr; bool owns; void* buf; void* alloc; std::string origin; };
            std::vector<LiveBufDiag> buffers_diag;
            {
                std::lock_guard lock(mutex);
                if (!live_buffer_wrappers.empty()) {
                    buffers_to_destroy.reserve(live_buffer_wrappers.size());
                    buffers_diag.reserve(live_buffer_wrappers.size());
                    for (auto ptr : live_buffer_wrappers) {
                        buffers_to_destroy.push_back(ptr);
                        auto* b = static_cast<VulkanBuffer*>(ptr);
                        LiveBufDiag d{ ptr, false, nullptr, nullptr, std::string() };
                        if (b) { d.owns = b->owns_allocation; d.buf = reinterpret_cast<void*>(b->buffer); d.alloc = reinterpret_cast<void*>(b->allocation); }
                        auto it = live_buffer_origins.find(ptr);
                        if (it != live_buffer_origins.end()) { d.origin = it->second; }
                        // Remove origin entry to avoid stale map entries
                        live_buffer_origins.erase(ptr);
                        buffers_diag.push_back(d);
                    }
                    live_buffer_wrappers.clear();
                }
                if (!live_image_wrappers.empty()) {
                    images_to_destroy.reserve(live_image_wrappers.size());
                    for (auto ptr : live_image_wrappers) images_to_destroy.push_back(ptr);
                    live_image_wrappers.clear();
                }
            }

            // Dump diagnostic info for live buffers we are about to destroy.
            if (!buffers_diag.empty()) {
                for (const auto &d : buffers_diag) {
                    bud::eprint("[VulkanMemoryAllocator] live buffer wrapper: ptr={} owns_allocation={} VkBuffer={} allocation={} origin={}", (void*)d.ptr, d.owns, d.buf, d.alloc, d.origin);
                }
            }

            if (!buffers_to_destroy.empty()) {
                bud::print("[VulkanMemoryAllocator] Forcibly destroying {} live buffer wrappers before allocator destroy.", buffers_to_destroy.size());
                for (auto ptr : buffers_to_destroy) {
                    auto* b = static_cast<VulkanBuffer*>(ptr);
                    if (b) {
                        // Destroy the underlying VMA allocation if owned and clear fields
                        if (b->owns_allocation && b->buffer != VK_NULL_HANDLE && b->allocation != VK_NULL_HANDLE) {
                            vmaDestroyBuffer(vma_allocator, b->buffer, b->allocation);
                        }
                        // Clear ownership so any remaining shared_ptr owners won't attempt to destroy again.
                        b->owns_allocation = false;
                        b->buffer = VK_NULL_HANDLE;
                        b->allocation = VK_NULL_HANDLE;
                        b->mapped_ptr = nullptr;
                        b->size = 0;
                        // Do NOT delete the wrapper object here - other shared owners may still exist.
                    }
                }
            }

            if (!images_to_destroy.empty()) {
                bud::print("[VulkanMemoryAllocator] Forcibly destroying {} live image wrappers before allocator destroy.", images_to_destroy.size());
                for (auto ptr : images_to_destroy) {
                    auto* t = static_cast<VulkanTexture*>(ptr);
                    if (t) {
                        if (t->image != VK_NULL_HANDLE && t->allocation != VK_NULL_HANDLE) {
                            vmaDestroyImage(vma_allocator, t->image, t->allocation);
                        }
                        // Clear fields so later destructors won't double-free
                        t->image = VK_NULL_HANDLE;
                        t->allocation = VK_NULL_HANDLE;
                        if (t->view) { vkDestroyImageView(device, t->view, nullptr); t->view = VK_NULL_HANDLE; }
                        t->sampler = VK_NULL_HANDLE;
                        // Do NOT delete the wrapper object here.
                    }
                }
            }
#else
            // Tracking disabled: no diagnostics collected.
#endif

            // Always destroy the VMA allocator now that we've optionally handled diagnostics.
            vmaDestroyAllocator(vma_allocator);
            vma_allocator = nullptr;
        }
    }

    void VulkanMemoryAllocator::on_frame_begin(uint32_t frame_index) {
        // Move deferred lists out under lock, then perform owner.reset() outside
        // to avoid invoking user-defined deleters while holding allocator mutex
        // (those deleters may call back into this allocator and attempt to lock
        // the same mutex, causing deadlock / exceptions).
        std::vector<bud::graphics::BufferHandle> to_free_buffers;
        std::vector<bud::graphics::Texture*> to_free_textures;
        VmaLinearPage staging_copy;

        {
            std::lock_guard lock(mutex);
            // bud::print("[VulkanMemoryAllocator::on_frame_begin] frame_index={}", frame_index); // [LOG REDUCED]
            current_frame_index = frame_index;

            if (frame_index < deferred_free_buffers.size()) {
                to_free_buffers = std::move(deferred_free_buffers[frame_index]);
                deferred_free_buffers[frame_index].clear();
            }

            if (frame_index < deferred_free_textures.size()) {
                to_free_textures = std::move(deferred_free_textures[frame_index]);
                deferred_free_textures[frame_index].clear();
            }

            if (frame_index < staging_pages.size()) {
                // Copy minimal data if needed; resetting page is safe outside lock.
                staging_copy = staging_pages[frame_index];
                staging_pages[frame_index].reset();
            }
        }

        // Perform resets and owner releasing outside lock to avoid reentrancy deadlocks.
        for (auto& handle : to_free_buffers) {
            if (!handle.is_valid())
                continue;
            if (handle.owner) {
                handle.owner.reset();
            }
            handle.internal_state = nullptr;
        }

        // For textures we don't own full lifecycle; simply clear pointers
        for (auto* tex : to_free_textures) {
            (void)tex; // nothing to do here for now
        }

        (void)staging_copy; // placeholder to silence unused warning
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_frame_transient(uint64_t size, uint64_t alignment) {
        // Obsolete, transient resources are now created directly via VMA in RHI
        return {};
    }

    void VulkanMemoryAllocator::defer_free(bud::graphics::BufferHandle handle, uint32_t frame_index) {
        if (!handle.is_valid())
            return;
        std::lock_guard lock(mutex);
        if (deferred_free_buffers.empty())
            return;
        uint32_t idx = frame_index % deferred_free_buffers.size();
        // Move the handle into the deferred list to transfer ownership and avoid copies.
        deferred_free_buffers[idx].push_back(std::move(handle));
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
        auto sp = std::make_shared<VulkanBuffer>();
        auto* vk_buf = sp.get();
        vk_buf->buffer = current_page->buffer;
        vk_buf->allocation = current_page->allocation;
        vk_buf->owns_allocation = false; // It's a sub-allocation, so it shouldn't destroy the parent buffer
        vk_buf->allocator = vma_allocator;
            // Provide a small owning shared_ptr for the VulkanBuffer object itself so the
            // temporary VulkanBuffer struct is freed when the deferred handle is destroyed.
            // Note: this shared_ptr DOES NOT own the underlying VMA allocation (owns_allocation=false),
            // it only manages the lifetime of the VulkanBuffer helper object to avoid leaks.
            block.internal_state = vk_buf;
            // transfer shared ownership of wrapper object to handle.owner
            block.owner = std::static_pointer_cast<void>(sp);
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

        auto sp = std::make_shared<VulkanBuffer>();
        auto* vk_buf = sp.get();
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
        vk_buf->allocator = vma_allocator;
        vk_buf->owning_allocator = this;
        block.internal_state = vk_buf;
        // Track wrapper pointer so cleanup can forcibly free any remaining allocations
#ifdef BUD_VMA_TRACKING
        register_allocation_buffer(vk_buf);
#endif
        // transfer shared ownership into handle so destructor runs automatically
        block.owner = std::static_pointer_cast<void>(sp);
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
            std::string err = std::format("VulkanMemoryAllocator::alloc_gpu vmaCreateBuffer failed (size={} usage={})", size, (int)buffer_info.usage);
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

#ifdef BUD_VMA_TRACKING
        bud::print("[VulkanMemoryAllocator][alloc_gpu] VkBuffer={} size={} usage={} (ResourceState={})", (void*)vk_buf->buffer, size, (uint32_t)buffer_info.usage, (int)usage);
#endif

        bud::graphics::BufferHandle handle;
        handle.internal_state = vk_buf;
        register_allocation_buffer(vk_buf);
        handle.owner = std::shared_ptr<void>(vk_buf, [this, alloc = vma_allocator](void* p) {
            auto* b = static_cast<VulkanBuffer*>(p);
                this->unregister_allocation_buffer(b);
            if (b->owns_allocation && alloc && b->buffer != VK_NULL_HANDLE && b->allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(alloc, b->buffer, b->allocation);
            }
            delete b;
        });
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
            std::string err = std::format("VulkanMemoryAllocator::alloc_persistent vmaCreateBuffer failed (size={} usage={})", size, (int)buffer_info.usage);
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

#ifdef BUD_VMA_TRACKING
        bud::print("[VulkanMemoryAllocator][alloc_persistent] VkBuffer={} size={} usage={} (ResourceState={})", (void*)vk_buf->buffer, size, (uint32_t)buffer_info.usage, (int)usage);
#endif

        bud::graphics::BufferHandle handle;
        handle.internal_state = vk_buf;
        register_allocation_buffer(vk_buf);
        handle.owner = std::shared_ptr<void>(vk_buf, [this, alloc = vma_allocator](void* p) {
            auto* b = static_cast<VulkanBuffer*>(p);
                this->unregister_allocation_buffer(b);
            if (b->owns_allocation && alloc && b->buffer != VK_NULL_HANDLE && b->allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(alloc, b->buffer, b->allocation);
            }
            delete b;
        });
        handle.size = size;
        handle.mapped_ptr = vk_buf->mapped_ptr;
        return handle;
    }

    bud::graphics::BufferHandle VulkanMemoryAllocator::alloc_read_back(uint64_t size) {
        VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size = size;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; // Copy destination

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;

        auto sp = std::make_shared<VulkanBuffer>();
        auto* vk_buf = sp.get();
        VmaAllocationInfo alloc_result_info;
        if (vmaCreateBuffer(vma_allocator, &buffer_info, &alloc_info, &vk_buf->buffer, &vk_buf->allocation, &alloc_result_info) != VK_SUCCESS) {
            delete vk_buf;
            std::string err = std::format("VulkanMemoryAllocator::alloc_read_back vmaCreateBuffer failed (size={})", size);
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

#ifdef BUD_VMA_TRACKING
        bud::print("[VulkanMemoryAllocator][alloc_read_back] VkBuffer={} size={}", (void*)vk_buf->buffer, size);
#endif

        bud::graphics::BufferHandle handle;
        handle.internal_state = vk_buf;
        register_allocation_buffer(vk_buf);
        handle.owner = std::shared_ptr<void>(vk_buf, [this, alloc = vma_allocator](void* p) {
            auto* b = static_cast<VulkanBuffer*>(p);
                this->unregister_allocation_buffer(b);
            if (b->owns_allocation && alloc && b->buffer != VK_NULL_HANDLE && b->allocation != VK_NULL_HANDLE) {
                vmaDestroyBuffer(alloc, b->buffer, b->allocation);
            }
            delete b;
        });
        handle.size = size;
        handle.mapped_ptr = vk_buf->mapped_ptr;
        return handle;
    }

    bud::graphics::Texture* VulkanMemoryAllocator::create_texture(const bud::graphics::TextureDesc& desc) {
        return nullptr; // Stub for Phase 1
    }
}
