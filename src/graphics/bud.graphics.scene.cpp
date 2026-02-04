#include "src/graphics/bud.graphics.scene.hpp"

namespace bud::graphics {

	RenderScene::RenderScene(RenderScene&& other) noexcept {
		*this = std::move(other);
	}

	// 3. 移动赋值运算符 (Move Assignment)
	RenderScene& RenderScene::operator=(RenderScene&& other) noexcept {
		if (this != &other) {
			world_matrices = std::move(other.world_matrices);
			world_aabbs = std::move(other.world_aabbs);
			mesh_indices = std::move(other.mesh_indices);
			material_indices = std::move(other.material_indices);
			flags = std::move(other.flags);

			// Atomic 不能移动，我们只能把它的值读出来赋给新的对象
			// 对于 RenderScene 来说，resize 通常发生在这一帧开始前，count 应该是 0
			// 但为了正确性，我们 copy 这个值
			size_t val = other.instance_count.load(std::memory_order_relaxed);
			instance_count.store(val, std::memory_order_relaxed);
		}
		return *this;
	}

	void RenderScene::reset(size_t estimated_capacity) {
		instance_count.store(0, std::memory_order_relaxed);

		constexpr size_t buffering_size = 128;

		if (estimated_capacity > world_matrices.capacity()) {
			world_matrices.resize(estimated_capacity + buffering_size); // 多分配一点做缓冲
			world_aabbs.resize(estimated_capacity + buffering_size);
			mesh_indices.resize(estimated_capacity + buffering_size);
			material_indices.resize(estimated_capacity + buffering_size);
		}

		if (estimated_capacity > flags.capacity()) {
			flags.resize(estimated_capacity + buffering_size);
		}

		if (world_matrices.size() < estimated_capacity) {
			world_matrices.resize(estimated_capacity);
			world_aabbs.resize(estimated_capacity);
			mesh_indices.resize(estimated_capacity);
			material_indices.resize(estimated_capacity);
			flags.resize(estimated_capacity);
		}
	}

	// 获取当前有效数量
	inline size_t RenderScene::size() const {
		return instance_count.load(std::memory_order_relaxed);
	}
}
