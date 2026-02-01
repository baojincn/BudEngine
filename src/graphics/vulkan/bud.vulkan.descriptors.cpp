#include <vulkan/vulkan.h>
#include <vector>
#include <algorithm>

#include "src/graphics/vulkan/bud.vulkan.descriptors.hpp"

namespace bud::graphics::vulkan {

    // --- VulkanDescriptorAllocator ---

    void VulkanDescriptorAllocator::init(VkDevice device) {
        this->device = device;
    }

    void VulkanDescriptorAllocator::cleanup() {
        for (auto p : free_pools) {
            vkDestroyDescriptorPool(device, p, nullptr);
        }
        for (auto p : used_pools) {
            vkDestroyDescriptorPool(device, p, nullptr);
        }
        free_pools.clear();
        used_pools.clear();
    }

    void VulkanDescriptorAllocator::reset_frame() {
        for (auto p : used_pools) {
            vkResetDescriptorPool(device, p, 0);
            free_pools.push_back(p);
        }
        used_pools.clear();
        current_pool = VK_NULL_HANDLE;
    }

    bool VulkanDescriptorAllocator::allocate(VkDescriptorSetLayout layout, VkDescriptorSet& out_set) {
        if (current_pool == VK_NULL_HANDLE) {
            current_pool = grab_pool();
            used_pools.push_back(current_pool);
        }

        VkDescriptorSetAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.pNext = nullptr;
        alloc_info.pSetLayouts = &layout;
        alloc_info.descriptorPool = current_pool;
        alloc_info.descriptorSetCount = 1;

        VkResult res = vkAllocateDescriptorSets(device, &alloc_info, &out_set);

        if (res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL) {
            current_pool = grab_pool();
            used_pools.push_back(current_pool);
            alloc_info.descriptorPool = current_pool;

            return vkAllocateDescriptorSets(device, &alloc_info, &out_set) == VK_SUCCESS;
        }

        return res == VK_SUCCESS;
    }

    VkDescriptorPool VulkanDescriptorAllocator::grab_pool() {
        if (!free_pools.empty()) {
            VkDescriptorPool pool = free_pools.back();
            free_pools.pop_back();
            return pool;
        }
        else {
            return create_pool(1000, 0);
        }
    }

    VkDescriptorPool VulkanDescriptorAllocator::create_pool(uint32_t count, uint32_t flags) {
        std::vector<VkDescriptorPoolSize> sizes;
        sizes.reserve(11);
        sizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLER, (uint32_t)(count * 0.5f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(count * 4.0f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)(count * 2.0f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)(count * 2.0f) });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, count });
        sizes.push_back({ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, count });

        VkDescriptorPoolCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = flags;
        info.maxSets = count;
        info.poolSizeCount = (uint32_t)sizes.size();
        info.pPoolSizes = sizes.data();

        VkDescriptorPool pool;
        vkCreateDescriptorPool(device, &info, nullptr, &pool);

        return pool;
    }

    // --- DescriptorLayoutBuilder ---

    void DescriptorLayoutBuilder::add_binding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags, uint32_t count, VkDescriptorBindingFlags bindingFlags) {
        Binding new_bind{};
        new_bind.binding = binding;
        new_bind.count = count;
        new_bind.type = type;
        new_bind.stage_flags = stageFlags;
        new_bind.binding_flags = bindingFlags;

        bindings.push_back(new_bind);
    }

    void DescriptorLayoutBuilder::clear() {
        bindings.clear();
    }

    VkDescriptorSetLayout DescriptorLayoutBuilder::build(VkDevice device, VkShaderStageFlags shader_stages, void* pNext, VkDescriptorSetLayoutCreateFlags flags) {
        std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
        std::vector<VkDescriptorBindingFlags> binding_flags;
        bool has_flags = false;

        for (auto& b : bindings) {
            VkDescriptorSetLayoutBinding bind{};
            bind.binding = b.binding;
            bind.descriptorCount = b.count;
            bind.descriptorType = b.type;
            bind.stageFlags = b.stage_flags | shader_stages; // Merge default stages
            
            vk_bindings.push_back(bind);
            binding_flags.push_back(b.binding_flags);
            if (b.binding_flags != 0) has_flags = true;
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flagsInfo.bindingCount = (uint32_t)binding_flags.size();
        flagsInfo.pBindingFlags = binding_flags.data();
        flagsInfo.pNext = pNext;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.pNext = has_flags ? &flagsInfo : pNext;
        info.pBindings = vk_bindings.data();
        info.bindingCount = (uint32_t)vk_bindings.size();
        info.flags = flags;

        // If any binding has UPDATE_AFTER_BIND, the layout must have it too
        for(auto f : binding_flags) {
            if (f & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) {
                info.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
                break;
            }
        }

        VkDescriptorSetLayout set;
        vkCreateDescriptorSetLayout(device, &info, nullptr, &set);

        return set;
    }

    // --- DescriptorWriter ---

    void DescriptorWriter::write_image(int binding, int arrayElement, VkImageView image, VkSampler sampler, VkImageLayout layout, VkDescriptorType type) {
        VkDescriptorImageInfo& info = image_infos.emplace_back(VkDescriptorImageInfo{
            .sampler = sampler,
            .imageView = image,
            .imageLayout = layout
        });

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = binding;
        write.dstSet = VK_NULL_HANDLE; // Updated later
        write.dstArrayElement = arrayElement;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pImageInfo = &info;

        writes.push_back(write);
    }

    void DescriptorWriter::write_buffer(int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type) {
        VkDescriptorBufferInfo& info = buffer_infos.emplace_back(VkDescriptorBufferInfo{
            .buffer = buffer,
            .offset = offset,
            .range = size
        });

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = binding;
        write.dstSet = VK_NULL_HANDLE; // Updated later
        write.dstArrayElement = 0;
        write.descriptorCount = 1;
        write.descriptorType = type;
        write.pBufferInfo = &info;

        writes.push_back(write);
    }

    void DescriptorWriter::clear() {
        image_infos.clear();
        buffer_infos.clear();
        writes.clear();
    }

    void DescriptorWriter::update_set(VkDevice device, VkDescriptorSet set) {
        for (VkWriteDescriptorSet& write : writes) {
            write.dstSet = set;
        }

        vkUpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

}
