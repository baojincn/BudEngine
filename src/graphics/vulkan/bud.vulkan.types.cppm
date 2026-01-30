module;

#include <vector>
#include <optional>
#include <vulkan/vulkan.h>

export module bud.vulkan.types;

import bud.math;
import bud.graphics.defs;
import bud.graphics.types;

export namespace bud::graphics::vulkan {
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
		VkSampler sampler = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		bud::graphics::MemoryBlock memory_block; // Store block for allocator->free()
	};

	struct VulkanLayoutTransition {
		VkImageLayout layout;
		VkAccessFlags access;
		VkPipelineStageFlags stage;
	};

	// Strict alignment for UBO
	struct UniformBufferObject {
		alignas(16) bud::math::mat4 view;
		alignas(16) bud::math::mat4 proj;

		alignas(16) bud::math::mat4 cascade_view_proj[MAX_CASCADES];
		alignas(16) bud::math::vec4 cascade_split_depths; // Pack 4 depths into vec4 (x,y,z,w)

		alignas(16) bud::math::vec3 cam_pos;
		alignas(16) bud::math::vec3 light_dir;
		alignas(16) bud::math::vec3 light_color;
		float light_intensity;
		float ambient_strength;
		uint32_t cascade_count;
		uint32_t debug_cascades;
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
