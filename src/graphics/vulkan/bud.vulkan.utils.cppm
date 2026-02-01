module;
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <format>

export module bud.vulkan.utils;

import bud.graphics.types;

export namespace bud::graphics::vulkan {

	constexpr VkObjectType to_vk_object_type(bud::graphics::ObjectType type) {
		switch (type) {
		case ObjectType::Texture:       return VK_OBJECT_TYPE_IMAGE;
		case ObjectType::ImageView:     return VK_OBJECT_TYPE_IMAGE_VIEW;
		case ObjectType::Buffer:        return VK_OBJECT_TYPE_BUFFER;
		case ObjectType::Shader:        return VK_OBJECT_TYPE_SHADER_MODULE;
		case ObjectType::Pipeline:      return VK_OBJECT_TYPE_PIPELINE;
		case ObjectType::CommandBuffer: return VK_OBJECT_TYPE_COMMAND_BUFFER;
		case ObjectType::Queue:         return VK_OBJECT_TYPE_QUEUE;
		case ObjectType::Semaphore:     return VK_OBJECT_TYPE_SEMAPHORE;
		case ObjectType::Fence:         return VK_OBJECT_TYPE_FENCE;
		case ObjectType::Sampler:       return VK_OBJECT_TYPE_SAMPLER;
		case ObjectType::Instance:      return VK_OBJECT_TYPE_INSTANCE;
		case ObjectType::Device:        return VK_OBJECT_TYPE_DEVICE;
		case ObjectType::RenderPass:    return VK_OBJECT_TYPE_RENDER_PASS;
		case ObjectType::DescriptorSet: return VK_OBJECT_TYPE_DESCRIPTOR_SET;
		default:                        return VK_OBJECT_TYPE_UNKNOWN;
		}
	}

	// --- 1. 格式转换 ---
	constexpr VkFormat to_vk_format(TextureFormat format) {
		switch (format) {
		case TextureFormat::Undefined:         return VK_FORMAT_UNDEFINED; // [FIX] Allow Undefined
		case TextureFormat::RGBA8_UNORM:       return VK_FORMAT_R8G8B8A8_SRGB;
		case TextureFormat::BGRA8_UNORM:       return VK_FORMAT_B8G8R8A8_UNORM;
		case TextureFormat::BGRA8_SRGB:        return VK_FORMAT_B8G8R8A8_SRGB; // [FIX] match Swapchain
		case TextureFormat::R32G32B32_FLOAT:   return VK_FORMAT_R32G32B32_SFLOAT;
		case TextureFormat::D32_FLOAT:         return VK_FORMAT_D32_SFLOAT;
		case TextureFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
		default: throw std::runtime_error("Unsupported TextureFormat");
		}
	}

	// --- 2. 自动推导 Image Aspect (深度/颜色) ---
	constexpr VkImageAspectFlags get_aspect_flags(VkFormat format) {
		switch (format) {
		case VK_FORMAT_D32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D16_UNORM_S8_UINT:
			// 简单的深度判断，如果包含 Stencil 应该加上 STENCIL_BIT
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		default:
			return VK_IMAGE_ASPECT_COLOR_BIT;
		}
	}

	// --- 3. 自动推导 Usage Flags ---
	// 根据用途推断 Vulkan Usage
	constexpr VkImageUsageFlags get_image_usage(VkFormat format, bool is_storage = false) {
		VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |      // 总是可以被采样
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | // 总是可以作为拷贝源
			VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // 总是可以作为拷贝目标

		if (get_aspect_flags(format) & VK_IMAGE_ASPECT_DEPTH_BIT) {
			usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		}
		else {
			usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		}

		if (is_storage) {
			usage |= VK_IMAGE_USAGE_STORAGE_BIT;
		}

		return usage;
	}

	// --- 4. 状态转换 (Barrier 用) ---
	// 这个之前写在 RHI 里，现在搬过来统一管理
	struct VulkanLayoutTransition {
		VkImageLayout layout;
		VkAccessFlags access;
		VkPipelineStageFlags stage;
	};

	constexpr VulkanLayoutTransition to_vk_transition(ResourceState state) {
		switch (state) {
		case ResourceState::Undefined:
			return { VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT };

		case ResourceState::RenderTarget:
			return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
					 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };

		case ResourceState::DepthWrite:
			return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
					 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };

		case ResourceState::DepthRead:
			return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
					 VK_ACCESS_SHADER_READ_BIT,
					 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT };

		case ResourceState::ShaderResource:
			return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					 VK_ACCESS_SHADER_READ_BIT,
					 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT };

		case ResourceState::TransferDst:
			return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					 VK_ACCESS_TRANSFER_WRITE_BIT,
					 VK_PIPELINE_STAGE_TRANSFER_BIT };

		case ResourceState::Present:
			return { VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
					 0,
					 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };

		default:
			throw std::runtime_error("Unknown ResourceState transition");
		}
	}
}
