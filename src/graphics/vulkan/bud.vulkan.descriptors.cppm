module;
#include <vulkan/vulkan.h>
#include <vector>
#include <deque>

export module bud.vulkan.descriptors;

namespace bud::graphics::vulkan {

	export class VulkanDescriptorAllocator {
	public:
		struct PoolSizeRatio {
			VkDescriptorType type;
			float ratio;
		};

		void init(VkDevice device);
		void cleanup();
		void reset_frame();
		bool allocate(VkDescriptorSetLayout layout, VkDescriptorSet& out_set);

		VkDevice device;

	private:
		VkDescriptorPool grab_pool();
		VkDescriptorPool create_pool(uint32_t count, uint32_t flags);

		VkDescriptorPool current_pool = VK_NULL_HANDLE;
		std::vector<VkDescriptorPool> used_pools;
		std::vector<VkDescriptorPool> free_pools;
	};

	export class DescriptorLayoutBuilder {
	public:
		struct Binding {
			uint32_t binding;
			VkDescriptorType type;
			uint32_t count;
			VkShaderStageFlags stage_flags;
			VkDescriptorBindingFlags binding_flags;
		};

		std::vector<Binding> bindings;

		void add_binding(uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags = 0, uint32_t count = 1, VkDescriptorBindingFlags bindingFlags = 0);
		void clear();
		VkDescriptorSetLayout build(VkDevice device, VkShaderStageFlags shader_stages, void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0);
	};

	export class DescriptorWriter {
	public:
		std::deque<VkDescriptorImageInfo> image_infos;
		std::deque<VkDescriptorBufferInfo> buffer_infos;
		std::vector<VkWriteDescriptorSet> writes;

		// Updated to support writing to specific array elements
		void write_image(int binding, int arrayElement, VkImageView image, VkSampler sampler, VkImageLayout layout, VkDescriptorType type);
		void write_buffer(int binding, VkBuffer buffer, size_t size, size_t offset, VkDescriptorType type);

		void clear();
		void update_set(VkDevice device, VkDescriptorSet set);
	};
}
