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
        for (auto* tex : acquired_textures) {
            destroy_vulkan_objects(tex);
            delete tex;
        }
        acquired_textures.clear();

        for (auto& [hash, list] : image_pool) {
            // list 是 vector<unique_ptr<VulkanTexture>>
            // unique_ptr 会自动 delete 对象，但我们需要先手动销毁 Vulkan 句柄
            for (auto& tex : list) {
                destroy_vulkan_objects(tex.get());
            }
            list.clear();
        }
    }

    Texture* VulkanResourcePool::acquire_texture(const TextureDesc& desc) {
        size_t hash = hash_desc(desc);

        std::unique_ptr<VulkanTexture> tex;

        // 1. 尝试复用
        if (!image_pool[hash].empty()) {
            tex = std::move(image_pool[hash].back());
            image_pool[hash].pop_back();
        }
        else {
            // 2. 新建
            tex = create_texture_smart(desc);
        }

        // 将所有权暂时交给 raw pointer 返回给 RenderGraph
        // RenderGraph 必须保证调用 release_texture
        VulkanTexture* raw_ptr = tex.release();

        // [FIX] Track acquired textures for cleanup
        acquired_textures.insert(raw_ptr);

        return raw_ptr;
    }

    void VulkanResourcePool::release_texture(Texture* texture) {
        if (!texture) {
            std::string err = std::format("VulkanResourcePool::release_texture called with null texture");
            bud::eprint("{}", err);
#if defined(_DEBUG)
            throw std::runtime_error(err);
#else
            return;
#endif
        }

        auto vk_tex = static_cast<VulkanTexture*>(texture);

        // [FIX] Remove from acquired tracking
        acquired_textures.erase(vk_tex);

        // Use the stored hash to recycle
        size_t hash = vk_tex->desc_hash;
        if (hash != 0) {
            // Recycle
            image_pool[hash].push_back(std::unique_ptr<VulkanTexture>(vk_tex));
        }
        else {
            // Fallback if hash missing
            destroy_vulkan_objects(vk_tex);
            delete vk_tex;
        }
    }

    void VulkanResourcePool::tick() {
        // 定期清理过久的资源
    }

    size_t VulkanResourcePool::hash_desc(const TextureDesc& desc) {
        size_t h = desc.width ^ (desc.height << 1) ^ ((uint32_t)desc.format << 2) ^ (desc.mips << 3) ^ (desc.array_layers << 4) ^ ((uint32_t)desc.type << 5);
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
            vmaDestroyImage(allocator->get_vma_allocator(), tex->image, tex->allocation);
        }
    }

    std::unique_ptr<VulkanTexture> VulkanResourcePool::create_texture_smart(const TextureDesc& desc) {
        auto tex = std::make_unique<VulkanTexture>();

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
            throw std::runtime_error("Failed to create pooled image");
        }
        // bud::print("[Pool] Created VkImage {} ({}x{} fmt={} layers={})", (void*)tex->image, desc.width, desc.height, (int)desc.format, desc.array_layers);

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
            throw std::runtime_error("Failed to create image view");
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
                    throw std::runtime_error("Failed to create layer image view");
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
                    throw std::runtime_error("Failed to create mip image view");
                }
            }
        }

        return tex;
    }

}
