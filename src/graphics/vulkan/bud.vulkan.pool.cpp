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
        // [FIX] Clean up any acquired textures that weren't released
        for (auto& sp : acquired_textures_shared) {
            if (sp) {
                destroy_vulkan_objects(static_cast<VulkanTexture*>(sp.get()));
            }
        }
        acquired_textures_shared.clear();

        for (auto& [hash, list] : image_pool) {
            // list 是 vector<shared_ptr<VulkanTexture>>
            for (auto& tex : list) {
                if (tex) destroy_vulkan_objects(tex.get());
            }
            list.clear();
        }
    }

    Texture* VulkanResourcePool::acquire_texture(const TextureDesc& desc) {
        // Backward-compatible raw API that returns a raw pointer but keeps a
        // managed shared_ptr internally. Prefer using acquire_texture_shared.
        auto sp = acquire_texture_shared(desc);
        return sp.get();
    }

    std::shared_ptr<Texture> VulkanResourcePool::acquire_texture_shared(const TextureDesc& desc) {
        size_t hash = hash_desc(desc);

        std::shared_ptr<VulkanTexture> tex;

        // 1. 尝试复用
        if (!image_pool[hash].empty()) {
            tex = image_pool[hash].back();
            image_pool[hash].pop_back();
        }
        else {
            // 2. 新建
            tex = create_texture_smart(desc);
        }

        // Track acquired textures
        acquired_textures_shared.push_back(tex);

        return tex;
    }

    void VulkanResourcePool::release_texture(Texture* texture) {
        // Keep backward compat: convert to shared_ptr release
        if (!texture) return;
        // Find shared_ptr in acquired_textures_shared
        for (auto it = acquired_textures_shared.begin(); it != acquired_textures_shared.end(); ++it) {
            if (it->get() == texture) {
                // Move ownership back into pool or destroy
                auto sp = *it;
                acquired_textures_shared.erase(it);
                // recycle into pool
                size_t hash = sp->desc_hash;
                if (hash != 0) {
                    image_pool[hash].push_back(sp);
                } else {
                    destroy_vulkan_objects(static_cast<VulkanTexture*>(sp.get()));
                }
                return;
            }
        }
    }

    void VulkanResourcePool::release_texture_shared(std::shared_ptr<Texture> texture) {
        if (!texture) return;
        // Find and remove from acquired list
        for (auto it = acquired_textures_shared.begin(); it != acquired_textures_shared.end(); ++it) {
            if (it->get() == texture.get()) {
                auto sp = *it;
                acquired_textures_shared.erase(it);
                size_t hash = sp->desc_hash;
                if (hash != 0) {
                    image_pool[hash].push_back(sp);
                } else {
                    destroy_vulkan_objects(static_cast<VulkanTexture*>(sp.get()));
                }
                return;
            }
        }
    }

    void VulkanResourcePool::tick() {
        // 定期清理过久的资源
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
            // bud::print("[Pool] Destroying VkImage {} ({}x{} fmt={})", (void*)tex->image, tex->width, tex->height, (int)tex->format);
            // Unregister from allocator tracking then destroy
            // Unregister wrapper (if tracked) and destroy image
            allocator->unregister_allocation_image(tex);
            vmaDestroyImage(allocator->get_vma_allocator(), tex->image, tex->allocation);
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
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

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

        // [FIX] For R32_SFLOAT (used by HiZ), map R to RGB channels so Nsight shows it as grayscale
        // rather than bright blinding red.
        if (vk_format == VK_FORMAT_R32_SFLOAT) {
            view_info.components.r = VK_COMPONENT_SWIZZLE_R;
            view_info.components.g = VK_COMPONENT_SWIZZLE_R;
            view_info.components.b = VK_COMPONENT_SWIZZLE_R;
            view_info.components.a = VK_COMPONENT_SWIZZLE_ONE;
        }

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

}
