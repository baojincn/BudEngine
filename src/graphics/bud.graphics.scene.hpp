#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <algorithm>

#include "src/core/bud.math.hpp"

namespace bud::threading {
	class TaskScheduler;
}

namespace bud::graphics {

	// SoA (Structure of Arrays) accelerate Cache effeciency and multi-threading processing
	struct RenderScene {
		std::vector<bud::math::mat4> world_matrices;
		std::vector<bud::math::AABB> world_aabbs;

		std::vector<uint32_t> mesh_indices;
		std::vector<uint32_t> submesh_indices;
		std::vector<uint32_t> material_indices;

		// 标志位 (Bit 0 = IsStatic, Bit 1 = CastShadow ...)
		std::vector<uint8_t> flags;

		struct LBVHNode {
			uint32_t instance_index;
			uint32_t morton_code;

			bool operator<(const LBVHNode& other) const {
				return morton_code < other.morton_code;
			}
		};

		struct BVHNode {
			bud::math::AABB aabb;
			uint32_t left_child{ ~0u };
			uint32_t right_child{ ~0u };
			uint32_t instance_index{ ~0u };
			bool is_leaf{ false };
		};

		std::vector<LBVHNode> lbvh_nodes;
		std::vector<BVHNode> bvh_nodes;
		bud::math::AABB scene_bounds;
		uint32_t bvh_root{ ~0u };

		std::atomic<size_t> instance_count{ 0 };
		std::atomic<size_t> dropped_instances{ 0 };

		RenderScene() = default;

		RenderScene(RenderScene&& other) noexcept;

		RenderScene& operator=(RenderScene&& other) noexcept;

		RenderScene(const RenderScene&) = delete;
		RenderScene& operator=(const RenderScene&) = delete;

		void reset(size_t estimated_capacity) {
			world_matrices.assign(estimated_capacity, bud::math::mat4(1.0f));
			world_aabbs.assign(estimated_capacity, bud::math::AABB());
			mesh_indices.assign(estimated_capacity, 0);
			submesh_indices.assign(estimated_capacity, 0);
			material_indices.assign(estimated_capacity, 0);
			flags.assign(estimated_capacity, 0);
			instance_count.store(0);
			dropped_instances.store(0);
		}
		void build_culling_lbvh();
		void build_culling_lbvh_parallel(bud::threading::TaskScheduler* task_scheduler);

		void cull_frustum(const bud::math::Frustum& frustum, std::vector<uint32_t>& out_indices) const;
		bool intersect_scene(const bud::math::AABB& aabb) const;

		inline void add_instance(const bud::math::mat4& transform, const bud::math::AABB& aabb, uint32_t mesh_index, uint32_t submesh_index, uint32_t material_index, bool is_static) {
			size_t idx = instance_count.fetch_add(1, std::memory_order_relaxed);

			if (idx >= world_matrices.size()) [[unlikely]] {
				dropped_instances.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			world_matrices[idx] = transform;
			world_aabbs[idx] = aabb;
			mesh_indices[idx] = mesh_index;
			submesh_indices[idx] = submesh_index;
			material_indices[idx] = material_index;

			flags[idx] = is_static ? 1 : 0;
		}

		inline size_t size() const {
			const size_t count = instance_count.load(std::memory_order_relaxed);
			return std::min(count, world_matrices.size());
		}
	};
}
