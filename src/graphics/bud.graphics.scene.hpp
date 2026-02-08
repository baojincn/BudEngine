#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <algorithm>

#include "src/core/bud.math.hpp"

namespace bud::graphics {

	// SoA (Structure of Arrays) accelerate Cache effeciency and multi-threading processing
	struct RenderScene {
		std::vector<bud::math::mat4> world_matrices;
		std::vector<bud::math::AABB> world_aabbs;

		std::vector<uint32_t> mesh_indices;
		std::vector<uint32_t> material_indices;

		// 标志位 (Bit 0 = IsStatic, Bit 1 = CastShadow ...)
		std::vector<uint8_t> flags;

		std::atomic<size_t> instance_count{ 0 };
		std::atomic<size_t> dropped_instances{ 0 };

		RenderScene() = default;

		RenderScene(RenderScene&& other) noexcept;

		RenderScene& operator=(RenderScene&& other) noexcept;

		RenderScene(const RenderScene&) = delete;
		RenderScene& operator=(const RenderScene&) = delete;

		void reset(size_t estimated_capacity);

		inline void add_instance(const bud::math::mat4& transform, const bud::math::AABB& aabb, uint32_t mesh_index, uint32_t material_index, bool is_static) {
			size_t idx = instance_count.fetch_add(1, std::memory_order_relaxed);

			if (idx >= world_matrices.size()) [[unlikely]] {
				dropped_instances.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			world_matrices[idx] = transform;
			world_aabbs[idx] = aabb;
			mesh_indices[idx] = mesh_index;
			material_indices[idx] = material_index;

			flags[idx] = is_static ? 1 : 0;
		}

		inline size_t size() const;
	};
}
