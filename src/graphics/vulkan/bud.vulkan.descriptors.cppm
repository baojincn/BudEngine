module;
#include <vulkan/vulkan.h>
#include <vector>
#include <deque>

export module bud.vulkan.descriptors;

namespace bud::graphics::vulkan {

	export class VulkanDescriptorAllocator {
	public:
		void init(VkDevice device) { m_device = device; }

		void cleanup() {
			for (auto p : m_free_pools) vkDestroyDescriptorPool(m_device, p, nullptr);
			for (auto p : m_used_pools) vkDestroyDescriptorPool(m_device, p, nullptr);
		}

		// 每一帧开始时调用，极其高效地重置所有池子
		void reset_frame() {
			for (auto p : m_used_pools) {
				vkResetDescriptorPool(m_device, p, 0);
				m_free_pools.push_back(p);
			}
			m_used_pools.clear();
			m_current_pool = VK_NULL_HANDLE;
		}

		bool allocate(VkDescriptorSetLayout layout, VkDescriptorSet& out_set) {
			if (m_current_pool == VK_NULL_HANDLE) {
				m_current_pool = grab_pool();
				m_used_pools.push_back(m_current_pool);
			}

			VkDescriptorSetAllocateInfo alloc_info{};
			alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			alloc_info.pSetLayouts = &layout;
			alloc_info.descriptorPool = m_current_pool;
			alloc_info.descriptorSetCount = 1;

			VkResult res = vkAllocateDescriptorSets(m_device, &alloc_info, &out_set);

			// 如果当前池子满了，换一个新的再试一次
			if (res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL) {
				m_current_pool = grab_pool();
				m_used_pools.push_back(m_current_pool);
				alloc_info.descriptorPool = m_current_pool;
				return vkAllocateDescriptorSets(m_device, &alloc_info, &out_set) == VK_SUCCESS;
			}
			return res == VK_SUCCESS;
		}

	private:
		VkDevice m_device;
		VkDescriptorPool m_current_pool = VK_NULL_HANDLE;
		std::vector<VkDescriptorPool> m_used_pools;
		std::vector<VkDescriptorPool> m_free_pools;

		VkDescriptorPool grab_pool() {
			if (!m_free_pools.empty()) {
				VkDescriptorPool pool = m_free_pools.back();
				m_free_pools.pop_back();
				return pool;
			}
			return create_pool(1000, 1000); // 每次申请能存 1000 个 Set 的池子
		}

		VkDescriptorPool create_pool(uint32_t count, uint32_t flags) {
			VkDescriptorPoolSize sizes[] = {
				{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
				{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
				// ... 添加你需要的大小
			};
			VkDescriptorPoolCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			info.maxSets = count;
			info.poolSizeCount = 2; // array size
			info.pPoolSizes = sizes;
			VkDescriptorPool pool;
			vkCreateDescriptorPool(m_device, &info, nullptr, &pool);
			return pool;
		}
	};
}
