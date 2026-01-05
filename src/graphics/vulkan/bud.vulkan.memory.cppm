export module bud.vulkan.memory;

import bud.graphics;

namespace bud::vulkan::memory {
	
	class VulkanMemoryManager : public bud::graphics::MemoryManager {
	public:
		void init() override {
			// ... Vulkan 特定的初始化 (create buffer, map memory) ...
		}

		bool allocate_staging(uint64_t size, bud::graphics::MemoryAllocation& out_alloc) override {
			// 将结果填入通用的 MemoryAllocation 结构
			/*out_alloc.mapped_ptr = my_ring_buffer.current_ptr;
			out_alloc.offset = my_ring_buffer.offset;*/
			// ...
			return true;
		}

		void mark_submitted(const bud::graphics::MemoryAllocation& alloc, void* sync_handle) override {
			// 将 void* 强转回 VkFence
		}

	private:
		// 这里包含具体的 StagingRingBuffer 实现细节
		//StagingRingBuffer internal_ring;
	};

}
