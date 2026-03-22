#include "src/graphics/vulkan/bud.vulkan.sync2.hpp"
#include <vulkan/vulkan.h>

namespace bud::graphics::vulkan::sync2 {

Transition2 get_transition2(ResourceState state) noexcept {
    switch (state) {
    case ResourceState::Undefined:
        return { VK_IMAGE_LAYOUT_UNDEFINED, static_cast<VkAccessFlags2>(0), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT };
    case ResourceState::RenderTarget:
        return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };
    case ResourceState::ShaderResource:
        return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT };
    case ResourceState::DepthWrite:
        return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, static_cast<VkPipelineStageFlags2>(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT) };
    case ResourceState::DepthRead:
        return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT };
    case ResourceState::Present:
        return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, static_cast<VkAccessFlags2>(0), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT };
    case ResourceState::TransferDst:
        return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT };
    case ResourceState::TransferSrc:
        return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT };
    case ResourceState::UnorderedAccess:
        return { VK_IMAGE_LAYOUT_GENERAL, static_cast<VkAccessFlags2>(VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT), VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT };
    case ResourceState::IndirectArgument:
        return { VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT };
    default:
        return { VK_IMAGE_LAYOUT_GENERAL, static_cast<VkAccessFlags2>(VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
    }
}

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
                        VkAccessFlags2 dstAccessMask) noexcept {

    VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = srcStageMask;
    barrier.srcAccessMask = srcAccessMask;
    barrier.dstStageMask = dstStageMask;
    barrier.dstAccessMask = dstAccessMask;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;

    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = 1;
    depInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &depInfo);
}

} // namespace bud::graphics::vulkan::sync2
