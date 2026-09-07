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
		alignas(16) bud::math::mat4 prev_view_proj;

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

		// --- appended (std140): per-cascade shadow metrics for the receiver bias ---
		// Offsets only grow, so shader variants that still declare the old, shorter
		// block keep working (the buffer is simply larger than their declared block).
		alignas(16) bud::math::vec4 cascade_texel_size;  // world metres per shadow texel
		alignas(16) bud::math::vec4 cascade_depth_range; // light-space slab thickness (m)
		float shadow_receiver_bias_texels;               // residual depth offset, in texels
		float shadow_normal_offset_texels;               // world-space SNO, in texels
		float _pad_shadow[2] = { 0.0f, 0.0f };           // pad to 16-byte boundary (584 + 8 = 592)

		// --- appended (std140): TAA parameters & reprojection matrices ---
		alignas(16) bud::math::mat4 unjittered_inv_view_proj;
		alignas(16) bud::math::mat4 prev_unjittered_view_proj;
		alignas(16) bud::math::vec4 jitter_offset;       // xy: pixel offset [-0.5, 0.5], zw: NDC offset
	};

	// Lock the std140 contract with the GLSL UBO copies (vg_common.glsl / forward_main.frag).
	// Any reordering that moves these offsets silently corrupts the shadow bias tail.
	static_assert(offsetof(UniformBufferObject, cascade_texel_size) == 544, "UBO: cascade_texel_size must stay at 544");
	static_assert(offsetof(UniformBufferObject, cascade_depth_range) == 560, "UBO: cascade_depth_range must stay at 560");
	static_assert(offsetof(UniformBufferObject, shadow_receiver_bias_texels) == 576, "UBO: shadow_receiver_bias_texels must stay at 576");
	static_assert(offsetof(UniformBufferObject, shadow_normal_offset_texels) == 580, "UBO: shadow_normal_offset_texels must stay at 580");
	static_assert(offsetof(UniformBufferObject, unjittered_inv_view_proj) == 592, "UBO: unjittered_inv_view_proj must stay at 592");
	static_assert(offsetof(UniformBufferObject, prev_unjittered_view_proj) == 656, "UBO: prev_unjittered_view_proj must stay at 656");
	static_assert(offsetof(UniformBufferObject, jitter_offset) == 720, "UBO: jitter_offset must stay at 720");
	static_assert(sizeof(UniformBufferObject) == 736, "UBO: std140 block size must be 736");

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
