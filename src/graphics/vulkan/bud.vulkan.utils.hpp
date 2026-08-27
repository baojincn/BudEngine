#pragma once

#include <vulkan/vulkan.h>
#include <stdexcept>
#include <format>

#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics::vulkan {

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

	// 格式转换
	constexpr VkFormat to_vk_format(TextureFormat format) {
		switch (format) {
		case TextureFormat::Undefined:         return VK_FORMAT_UNDEFINED;
		case TextureFormat::R8_UNORM:          return VK_FORMAT_R8_UNORM;
		case TextureFormat::RGBA8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
		case TextureFormat::RGBA8_SRGB:        return VK_FORMAT_R8G8B8A8_SRGB;
		case TextureFormat::BGRA8_UNORM:       return VK_FORMAT_B8G8R8A8_UNORM;
		case TextureFormat::BGRA8_SRGB:        return VK_FORMAT_B8G8R8A8_SRGB;
		case TextureFormat::BC7_UNORM:         return VK_FORMAT_BC7_UNORM_BLOCK;
		case TextureFormat::BC5_UNORM:         return VK_FORMAT_BC5_UNORM_BLOCK;
		case TextureFormat::RGBA16_FLOAT:      return VK_FORMAT_R16G16B16A16_SFLOAT;
		case TextureFormat::R32G32B32_FLOAT:   return VK_FORMAT_R32G32B32_SFLOAT;
		case TextureFormat::R32G32_UINT:       return VK_FORMAT_R32G32_UINT;
		case TextureFormat::RGBA32_UINT:      return VK_FORMAT_R32G32B32A32_UINT;
		case TextureFormat::D32_FLOAT:         return VK_FORMAT_D32_SFLOAT;
		case TextureFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
		case TextureFormat::R32_FLOAT:         return VK_FORMAT_R32_SFLOAT;
		default: throw std::runtime_error("Unsupported TextureFormat");
		}
	}

	// 自动推导 Image Aspect (深度/颜色)
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

	// 自动推导 Usage Flags
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

	constexpr VkBufferUsageFlags get_vk_buffer_usage(bud::graphics::ResourceState usage_state) {
		VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; // Allow both src & dst transfers by default
		if (usage_state == bud::graphics::ResourceState::VertexBuffer) {
			usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		} else if (usage_state == bud::graphics::ResourceState::IndexBuffer) {
			usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		} else if (usage_state == bud::graphics::ResourceState::IndirectArgument) {
			usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		} else if (usage_state == bud::graphics::ResourceState::UnorderedAccess) {
			usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		} else if (usage_state == bud::graphics::ResourceState::ShaderResource) {
			usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		} else if (usage_state == bud::graphics::ResourceState::Common || usage_state == bud::graphics::ResourceState::TransferDst) {
			usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		}
		return usage;
	}

    // 状态转换 (Barrier 用)
    // 迁移至 synchronization2 helpers：使用 sync2::get_transition2 / sync2::cmd_image_barrier2
}
