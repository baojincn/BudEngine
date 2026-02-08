#include "src/graphics/bud.graphics.scene.hpp"

namespace bud::graphics {

	RenderScene::RenderScene(RenderScene&& other) noexcept {
		*this = std::move(other);
	}

	RenderScene& RenderScene::operator=(RenderScene&& other) noexcept {
		if (this != &other) {
			world_matrices = std::move(other.world_matrices);
			world_aabbs = std::move(other.world_aabbs);
			mesh_indices = std::move(other.mesh_indices);
			material_indices = std::move(other.material_indices);
			flags = std::move(other.flags);

			instance_count.store(other.instance_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
			dropped_instances.store(other.dropped_instances.load(std::memory_order_relaxed), std::memory_order_relaxed);

			other.instance_count.store(0, std::memory_order_relaxed);
			other.dropped_instances.store(0, std::memory_order_relaxed);
		}
		return *this;
	}

	void RenderScene::reset(size_t estimated_capacity) {
		instance_count.store(0, std::memory_order_relaxed);
		dropped_instances.store(0, std::memory_order_relaxed);

		const size_t target = std::max(estimated_capacity, world_matrices.size());
		if (world_matrices.size() < target) {
			world_matrices.resize(target);
			world_aabbs.resize(target);
			mesh_indices.resize(target);
			material_indices.resize(target);
			flags.resize(target);
		}
	}

	inline size_t RenderScene::size() const {
		const size_t count = instance_count.load(std::memory_order_relaxed);
		return std::min(count, world_matrices.size());
	}
}
