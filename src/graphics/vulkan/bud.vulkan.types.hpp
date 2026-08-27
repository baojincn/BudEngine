#pragma once

#include <vector>
#include <optional>
#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics::vulkan {
	using VkInstance = struct VkInstance_T*;
	using VkPhysicalDevice = struct VkPhysicalDevice_T*;
	using VkDevice = struct VkDevice_T*;
	using VkQueue = struct VkQueue_T*;
	using VkSurfaceKHR = struct VkSurfaceKHR_T*;


	class VulkanTexture : public Texture {
	public:
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		std::vector<VkImageView> layer_views;
		std::vector<VkImageView> mip_views;
		VkSampler sampler = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
	};



	// Strict alignment for UBO.
	// IMPORTANT: GLSL std140 treats vec3 as occupying 16 bytes (same as vec4).
	// C++ vec3 is only 12 bytes, so we must add explicit float padding after
	// each vec3 field to keep C++ and GLSL offsets in sync. Failure to do so
	// shifts all following fields by 4 bytes per vec3, corrupting reads of
	// cascade_count and other scalars in GPU shaders.
	struct UniformBufferObject {
		alignas(16) bud::math::mat4 view;
		alignas(16) bud::math::mat4 proj;

		alignas(16) bud::math::mat4 cascade_view_proj[MAX_CASCADES];
		alignas(16) bud::math::vec4 cascade_split_depths; // Pack 4 depths into vec4 (x,y,z,w)

		alignas(16) bud::math::vec3 cam_pos;
		float _pad_cam_pos = 0.0f;         // pad vec3 → 16 bytes (std140)
		alignas(16) bud::math::vec3 light_dir;
		float _pad_light_dir = 0.0f;       // pad vec3 → 16 bytes (std140)
		alignas(16) bud::math::vec3 light_color;
		float light_intensity;             // packed with light_color as vec4.w
		float ambient_strength;
		uint32_t cascade_count;
		uint32_t debug_cascades;
		uint32_t reversed_z;
		float shadow_bias_constant;
		float shadow_bias_slope;
		uint32_t debug_cluster;
	};

	struct Vertex {
		float pos[3];
		float color[3];
		float normal[3];
		float uv[2];
		float tex_index;

		static VkVertexInputBindingDescription get_binding_description();

		static std::vector<VkVertexInputAttributeDescription> get_attribute_descriptions();
	};


	struct QueueFamilyIndices {
		std::optional<uint32_t> graphics_family;
		std::optional<uint32_t> present_family;
		// Dedicated transfer/copy queue family (pure VK_QUEUE_TRANSFER_BIT) so
		// uploads can run on a separate hardware queue. Falls back to the
		// graphics family with queue_index=1 when no dedicated family exists.
		std::optional<uint32_t> copy_family;
		uint32_t copy_queue_index = 0;
		// Dedicated async-compute queue family (VK_QUEUE_COMPUTE_BIT without
		// VK_QUEUE_GRAPHICS_BIT) so async compute passes can overlap graphics.
		std::optional<uint32_t> compute_family;
		uint32_t compute_queue_index = 0;

		bool is_complete() const {
			return graphics_family.has_value() && present_family.has_value();
		}
	};

	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> present_modes;
	};

	struct ShadowConstantData {
		bud::math::mat4 light_view_proj;
		bud::math::mat4 model;
		bud::math::vec4 light_dir;
	};
}
