#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory> 
#include <utility>
#include <stdexcept>

#include "src/graphics/vulkan/bud.vulkan.pool.hpp"
#include "src/graphics/vulkan/bud.vulkan.utils.hpp"
#include "src/graphics/vulkan/bud.vulkan.types.hpp"
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics::vulkan {

    VulkanResourcePool::VulkanResourcePool(VkDevice device, VulkanMemoryAllocator* allocator)
        : device(device), allocator(allocator) {
    }

    VulkanResourcePool::~VulkanResourcePool() {
        cleanup();
    }

    void VulkanResourcePool::cleanup() {
        std::lock_guard lock(mutex);
        for (auto& slot : texture_slots) {
            if (slot.in_use && slot.texture) {
                destroy_vulkan_objects(slot.texture.get());
                slot.texture.reset();
                slot.in_use = false;
            }
        }
        texture_slots.clear();
        free_texture_indices.clear();

        for (auto& [hash, list] : image_pool) {
            for (auto& tex : list) {
                if (tex) destroy_vulkan_objects(tex.get());
            }
            list.clear();
        }

        for (auto& slot : buffer_slots) {
            if (slot.in_use && slot.buffer) {
                destroy_vulkan_objects(slot.buffer.get());
                slot.buffer.reset();
                slot.in_use = false;
            }
        }
        buffer_slots.clear();
        free_buffer_indices.clear();

        for (auto& [hash, list] : buffer_pool) {
            for (auto& buf : list) {
                if (buf) destroy_vulkan_objects(buf.get());
            }
            list.clear();
        }
    }

    TextureHandle VulkanResourcePool::acquire_texture(const TextureDesc& desc) {
        std::lock_guard lock(mutex);
        size_t hash = hash_desc(desc);
        std::shared_ptr<VulkanTexture> tex;

        // 1. Try reusing from cache
        if (!image_pool[hash].empty()) {
            tex = image_pool[hash].back();
            image_pool[hash].pop_back();
        }
        else {
            // 2. Create new texture
            tex = create_texture_smart(desc);
        }

        if (!tex) {
            return TextureHandle{};
        }

        // 3. Allocate slot
        uint32_t slot_idx = 0;
        if (!free_texture_indices.empty()) {
            slot_idx = free_texture_indices.back();
            free_texture_indices.pop_back();
        }
        else {
            slot_idx = static_cast<uint32_t>(texture_slots.size());
            texture_slots.emplace_back();
        }

        auto& slot = texture_slots[slot_idx];
        slot.texture = tex;
        slot.in_use = true;

        return TextureHandle{ slot_idx };
    }

    void VulkanResourcePool::release_texture(TextureHandle handle) {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= texture_slots.size()) {
            return;
        }

        auto& slot = texture_slots[handle.id];
        if (!slot.in_use) {
            return;
        }

        auto tex = std::move(slot.texture);
        slot.texture.reset();
        slot.in_use = false;
        free_texture_indices.push_back(handle.id);

        if (tex) {
            size_t hash = tex->desc_hash;
            if (hash != 0) {
                image_pool[hash].push_back(std::move(tex));
            }
            else {
                destroy_vulkan_objects(tex.get());
            }
        }
    }

    Texture* VulkanResourcePool::get_texture(TextureHandle handle) {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= texture_slots.size()) {
            return nullptr;
        }
        const auto& slot = texture_slots[handle.id];
        if (!slot.in_use) {
            return nullptr;
        }
        return slot.texture.get();
    }

    const Texture* VulkanResourcePool::get_texture(TextureHandle handle) const {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= texture_slots.size()) {
            return nullptr;
        }
        const auto& slot = texture_slots[handle.id];
        if (!slot.in_use) {
            return nullptr;
        }
        return slot.texture.get();
    }

    TextureDesc VulkanResourcePool::get_texture_desc(TextureHandle handle) const {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= texture_slots.size()) {
            return TextureDesc{};
        }
        const auto& slot = texture_slots[handle.id];
        if (!slot.in_use || !slot.texture) {
            return TextureDesc{};
        }
        const Texture* tex = slot.texture.get();
        TextureDesc desc;
        desc.width = tex->width;
        desc.height = tex->height;
        desc.format = tex->format;
        desc.mips = tex->mips;
        desc.array_layers = tex->array_layers;
        desc.type = tex->type;
        return desc;
    }

    TextureHandle VulkanResourcePool::register_texture(std::shared_ptr<VulkanTexture> tex) {
        if (!tex) {
            return TextureHandle{};
        }
        std::lock_guard lock(mutex);
        uint32_t slot_idx = 0;
        if (!free_texture_indices.empty()) {
            slot_idx = free_texture_indices.back();
            free_texture_indices.pop_back();
        }
        else {
            slot_idx = static_cast<uint32_t>(texture_slots.size());
            texture_slots.emplace_back();
        }

        auto& slot = texture_slots[slot_idx];
        slot.texture = std::move(tex);
        slot.in_use = true;

        return TextureHandle{ slot_idx };
    }

    void VulkanResourcePool::tick() {
        // Periodic pool cleanup
    }

    size_t VulkanResourcePool::hash_desc(const TextureDesc& desc) {
        size_t h = desc.width ^ (desc.height << 1) ^ ((uint32_t)desc.format << 2) ^ (desc.mips << 3) ^ (desc.array_layers << 4) ^ ((uint32_t)desc.type << 5) ^ (desc.is_transfer_src ? 1 : 0 << 6);
        return h;
    }

    void VulkanResourcePool::destroy_vulkan_objects(VulkanTexture* tex) {
        if (!tex)
            return;
        for (auto v : tex->layer_views) {
            if (v) vkDestroyImageView(device, v, nullptr);
        }
        tex->layer_views.clear();

        for (auto v : tex->mip_views) {
            if (v) vkDestroyImageView(device, v, nullptr);
        }
        tex->mip_views.clear();
        if (tex->view) vkDestroyImageView(device, tex->view, nullptr);
        
        if (tex->image) {
            allocator->unregister_allocation_image(tex);
            if (tex->allocation != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator->get_vma_allocator(), tex->image, tex->allocation);
            }
            tex->image = VK_NULL_HANDLE;
            tex->allocation = VK_NULL_HANDLE;
        }
    }

    std::shared_ptr<VulkanTexture> VulkanResourcePool::create_texture_smart(const TextureDesc& desc) {
        auto tex = std::make_shared<VulkanTexture>();

        // 1. 填充基础信息
        tex->width = desc.width;
        tex->height = desc.height;
        tex->format = desc.format;
        tex->mips = desc.mips;
        tex->array_layers = desc.array_layers;
        tex->desc_hash = hash_desc(desc); // Store hash for recycling
        tex->current_state = desc.initial_state;

        // 2. 使用 Utils 转换参数
        auto vk_format = to_vk_format(desc.format);
        auto usage = get_image_usage(vk_format, desc.is_storage);
        if (desc.is_transfer_src) {
        	usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.extent = { desc.width, desc.height, 1 };
        image_info.mipLevels = desc.mips;
        image_info.arrayLayers = desc.array_layers;
        image_info.format = vk_format;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage = usage;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        std::vector<uint32_t> queue_family_indices;
        if (allocator && allocator->is_concurrent_sharing_enabled()) {
            queue_family_indices.push_back(allocator->get_graphics_family());
            if (allocator->get_compute_family() != allocator->get_graphics_family())
                queue_family_indices.push_back(allocator->get_compute_family());
            if (allocator->get_copy_family() != allocator->get_graphics_family() &&
                allocator->get_copy_family() != allocator->get_compute_family())
                queue_family_indices.push_back(allocator->get_copy_family());
        }

        if (queue_family_indices.size() > 1) {
            image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            image_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_indices.size());
            image_info.pQueueFamilyIndices = queue_family_indices.data();
        } else {
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        VmaAllocationCreateInfo alloc_info = {};
        alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(allocator->get_vma_allocator(), &image_info, &alloc_info, &tex->image, &tex->allocation, nullptr) != VK_SUCCESS) {
            std::string err = std::format("VulkanResourcePool::create_texture_smart failed to create image: {}x{} fmt={} layers={}", desc.width, desc.height, (int)desc.format, desc.array_layers);
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return nullptr;
#endif
        }
        // bud::print("[Pool] Created VkImage {} ({}x{} fmt={} layers={})", (void*)tex->image, desc.width, desc.height, (int)desc.format, desc.array_layers);

        // Register texture wrapper so allocator can free any leaked images at shutdown
        // We'll register the raw wrapper pointer; the allocator tracking stores
        // weak references to shared_ptr owners elsewhere.
        allocator->register_allocation_image(tex.get());

        // 5. Create View
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = tex->image;
        view_info.viewType = (desc.array_layers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = vk_format;
        view_info.subresourceRange.aspectMask = get_aspect_flags(vk_format);
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = desc.mips;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = desc.array_layers;

        // [FIX] Use identity swizzle for all views. Non-identity swizzle (RRR1) on
        // R32_SFLOAT views violates the Vulkan spec when the view is used as a
        // STORAGE_IMAGE or INPUT_ATTACHMENT descriptor. Layer and mip views below
        // also use identity swizzle for the same reason.
        view_info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

        if (vkCreateImageView(device, &view_info, nullptr, &tex->view) != VK_SUCCESS) {
            std::string err = std::format("VulkanResourcePool::create_texture_smart failed to create base image view for image {}", (void*)tex->image);
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            // Clean up image allocation
            if (tex->image) vmaDestroyImage(allocator->get_vma_allocator(), tex->image, tex->allocation);
            return nullptr;
#endif
        }

        // 6. [CSM] Create Layer Views (for rendering to individual layers)
        if (desc.array_layers > 1) {
            tex->layer_views.resize(desc.array_layers);
            for (uint32_t i = 0; i < desc.array_layers; ++i) {
                VkImageViewCreateInfo layer_view_info = view_info;
                layer_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                layer_view_info.subresourceRange.baseArrayLayer = i;
                layer_view_info.subresourceRange.layerCount = 1;

                // [FIX] Layer and Mip views are often used as STORAGE images (e.g. HiZ, CSM).
                // Vulkan requires IDENTITY swizzle for STORAGE descriptors.
                layer_view_info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

                if (vkCreateImageView(device, &layer_view_info, nullptr, &tex->layer_views[i]) != VK_SUCCESS) {
                    std::string err = std::format("VulkanResourcePool::create_texture_smart failed to create layer view {} for image {}", i, (void*)tex->image);
                    bud::eprint("{}", err);
#if defined(_DEBUG)
                    throw std::runtime_error(err);
#else
                    // cleanup
                    destroy_vulkan_objects(tex.get());
                    return nullptr;
#endif
                }
            }
        }

        // 7. Create Mip Views
        if (desc.mips > 1) {
            tex->mip_views.resize(desc.mips);
            for (uint32_t i = 0; i < desc.mips; ++i) {
                VkImageViewCreateInfo mip_view_info = view_info;
                mip_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                mip_view_info.subresourceRange.baseMipLevel = i;
                mip_view_info.subresourceRange.levelCount = 1;

                // [FIX] Layer and Mip views are often used as STORAGE images (e.g. HiZ).
                // Vulkan requires IDENTITY swizzle for STORAGE descriptors.
                mip_view_info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };

                if (vkCreateImageView(device, &mip_view_info, nullptr, &tex->mip_views[i]) != VK_SUCCESS) {
                    std::string err = std::format("VulkanResourcePool::create_texture_smart failed to create mip view {} for image {}", i, (void*)tex->image);
                    bud::eprint("{}", err);
#if defined(_DEBUG)
                    throw std::runtime_error(err);
#else
                    destroy_vulkan_objects(tex.get());
                    return nullptr;
#endif
                }
            }
        }

        return tex;
    }

    BufferHandle VulkanResourcePool::acquire_buffer(const BufferDesc& desc) {
        std::lock_guard lock(mutex);
        size_t hash = hash_desc(desc);
        std::shared_ptr<VulkanBuffer> buf;

        // 1. Try reusing from cache
        if (!buffer_pool[hash].empty()) {
            buf = buffer_pool[hash].back();
            buffer_pool[hash].pop_back();
        }
        else {
            // 2. Create new buffer
            buf = create_buffer_smart(desc);
        }

        if (!buf) {
            return BufferHandle{};
        }

        // 3. Allocate slot
        uint32_t slot_idx = 0;
        if (!free_buffer_indices.empty()) {
            slot_idx = free_buffer_indices.back();
            free_buffer_indices.pop_back();
        }
        else {
            slot_idx = static_cast<uint32_t>(buffer_slots.size());
            buffer_slots.emplace_back();
        }

        auto& slot = buffer_slots[slot_idx];
        slot.buffer = buf;
        slot.in_use = true;

        return BufferHandle{ slot_idx };
    }

    void VulkanResourcePool::release_buffer(BufferHandle handle) {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= buffer_slots.size()) {
            return;
        }

        auto& slot = buffer_slots[handle.id];
        if (!slot.in_use) {
            return;
        }

        auto buf = std::move(slot.buffer);
        slot.buffer.reset();
        slot.in_use = false;
        free_buffer_indices.push_back(handle.id);

        if (buf) {
            size_t hash = buf->desc_hash;
            if (hash != 0) {
                buffer_pool[hash].push_back(std::move(buf));
            }
            else {
                destroy_vulkan_objects(buf.get());
            }
        }
    }

    Buffer* VulkanResourcePool::get_buffer(BufferHandle handle) {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= buffer_slots.size()) {
            return nullptr;
        }
        const auto& slot = buffer_slots[handle.id];
        if (!slot.in_use) {
            return nullptr;
        }
        return slot.buffer.get();
    }

    const Buffer* VulkanResourcePool::get_buffer(BufferHandle handle) const {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= buffer_slots.size()) {
            return nullptr;
        }
        const auto& slot = buffer_slots[handle.id];
        if (!slot.in_use) {
            return nullptr;
        }
        return slot.buffer.get();
    }

    BufferDesc VulkanResourcePool::get_buffer_desc(BufferHandle handle) const {
        std::lock_guard lock(mutex);
        if (!handle.is_valid() || handle.id >= buffer_slots.size()) {
            return BufferDesc{};
        }
        const auto& slot = buffer_slots[handle.id];
        if (!slot.in_use || !slot.buffer) {
            return BufferDesc{};
        }
        const Buffer* buf = slot.buffer.get();
        BufferDesc desc;
        desc.size = buf->size;
        desc.usage = buf->usage;
        desc.memory_usage = buf->memory_usage;
        return desc;
    }

    BufferHandle VulkanResourcePool::register_buffer(std::shared_ptr<VulkanBuffer> buf) {
        if (!buf) {
            return BufferHandle{};
        }
        std::lock_guard lock(mutex);
        uint32_t slot_idx = 0;
        if (!free_buffer_indices.empty()) {
            slot_idx = free_buffer_indices.back();
            free_buffer_indices.pop_back();
        }
        else {
            slot_idx = static_cast<uint32_t>(buffer_slots.size());
            buffer_slots.emplace_back();
        }

        auto& slot = buffer_slots[slot_idx];
        slot.buffer = std::move(buf);
        slot.in_use = true;

        return BufferHandle{ slot_idx };
    }

    size_t VulkanResourcePool::hash_desc(const BufferDesc& desc) {
        size_t h = std::hash<uint64_t>{}(desc.size) ^ (static_cast<size_t>(desc.usage) << 1) ^ (static_cast<size_t>(desc.memory_usage) << 4);
        return h;
    }

    void VulkanResourcePool::destroy_vulkan_objects(VulkanBuffer* buf) {
        if (!buf) {
            return;
        }
        if (buf->buffer != VK_NULL_HANDLE) {
            if (allocator) {
                allocator->unregister_allocation_buffer(buf);
            }
            if (buf->owns_allocation && buf->allocation != VK_NULL_HANDLE && allocator) {
                vmaDestroyBuffer(allocator->get_vma_allocator(), buf->buffer, buf->allocation);
            }
            buf->buffer = VK_NULL_HANDLE;
            buf->allocation = VK_NULL_HANDLE;
            buf->mapped_ptr = nullptr;
        }
    }

    std::shared_ptr<VulkanBuffer> VulkanResourcePool::create_buffer_smart(const BufferDesc& desc) {
        auto buf = std::make_shared<VulkanBuffer>();
        buf->size = desc.size;
        buf->usage = desc.usage;
        buf->memory_usage = desc.memory_usage;
        buf->desc_hash = hash_desc(desc);
        buf->current_state = desc.usage;

        VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.size = desc.size;
        buffer_info.usage = get_vk_buffer_usage(desc.usage);
        if (desc.memory_usage == MemoryUsage::PersistentMapped || desc.memory_usage == MemoryUsage::StagingRing) {
            buffer_info.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        }
        else if (desc.memory_usage == MemoryUsage::Readback) {
            buffer_info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        std::vector<uint32_t> queue_family_indices;
        if (allocator && allocator->is_concurrent_sharing_enabled()) {
            queue_family_indices.push_back(allocator->get_graphics_family());
            if (allocator->get_compute_family() != allocator->get_graphics_family())
                queue_family_indices.push_back(allocator->get_compute_family());
            if (allocator->get_copy_family() != allocator->get_graphics_family() &&
                allocator->get_copy_family() != allocator->get_compute_family())
                queue_family_indices.push_back(allocator->get_copy_family());
        }

        if (queue_family_indices.size() > 1) {
            buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            buffer_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_family_indices.size());
            buffer_info.pQueueFamilyIndices = queue_family_indices.data();
        }
        else {
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        VmaAllocationCreateInfo alloc_info = {};
        if (desc.memory_usage == MemoryUsage::GpuOnly) {
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        }
        else if (desc.memory_usage == MemoryUsage::PersistentMapped || desc.memory_usage == MemoryUsage::StagingRing) {
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
            alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }
        else if (desc.memory_usage == MemoryUsage::Readback) {
            alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
            alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        }

        VmaAllocationInfo alloc_result_info;
        if (vmaCreateBuffer(allocator->get_vma_allocator(), &buffer_info, &alloc_info, &buf->buffer, &buf->allocation, &alloc_result_info) != VK_SUCCESS) {
            std::string err = std::format("VulkanResourcePool::create_buffer_smart failed: size={} usage={}", desc.size, (int)desc.usage);
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return nullptr;
#endif
        }

        buf->mapped_ptr = alloc_result_info.pMappedData;
        buf->allocator = allocator->get_vma_allocator();
        buf->owning_allocator = allocator;
        allocator->register_allocation_buffer(buf.get());
        return buf;
    }

}
