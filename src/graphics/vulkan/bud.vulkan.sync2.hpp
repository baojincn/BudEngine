#pragma once

#include <vulkan/vulkan.h>
#include "src/graphics/vulkan/bud.vulkan.types.hpp"

namespace bud::graphics::vulkan::sync2 {

struct Transition2 {
    VkImageLayout layout;
    VkAccessFlags2 access;
    VkPipelineStageFlags2 stage;
};

// Map engine ResourceState to synchronization2 transition info
Transition2 get_transition2(ResourceState state) noexcept;

// Issue an image barrier using synchronization2 (VkImageMemoryBarrier2 + vkCmdPipelineBarrier2)
void cmd_image_barrier2(VkCommandBuffer cmd,
                        VkImage image,
                        VkImageAspectFlags aspectMask,
                        uint32_t baseMipLevel,
                        uint32_t levelCount,
                        uint32_t baseArrayLayer,
                        uint32_t layerCount,
                        VkImageLayout oldLayout,
                        VkImageLayout newLayout,
                        VkPipelineStageFlags2 srcStageMask,
                        VkAccessFlags2 srcAccessMask,
                        VkPipelineStageFlags2 dstStageMask,
                        VkAccessFlags2 dstAccessMask) noexcept;

} // namespace bud::graphics::vulkan::sync2
