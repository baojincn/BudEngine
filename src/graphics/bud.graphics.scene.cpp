#include "src/graphics/bud.graphics.scene.hpp"
#include <bit>

#include "src/threading/bud.threading.hpp"

namespace bud::graphics {

	RenderScene::RenderScene(RenderScene&& other) noexcept {
		*this = std::move(other);
	}

	RenderScene& RenderScene::operator=(RenderScene&& other) noexcept {
		if (this != &other) {
			world_matrices = std::move(other.world_matrices);
			world_aabbs = std::move(other.world_aabbs);
			mesh_indices = std::move(other.mesh_indices);
			submesh_indices = std::move(other.submesh_indices);
			material_indices = std::move(other.material_indices);
			flags = std::move(other.flags);
			lbvh_nodes = std::move(other.lbvh_nodes);
			bvh_nodes = std::move(other.bvh_nodes);
			scene_bounds = std::move(other.scene_bounds);
			bvh_root = other.bvh_root;
			other.bvh_root = ~0u;

			instance_count.store(other.instance_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
			dropped_instances.store(other.dropped_instances.load(std::memory_order_relaxed), std::memory_order_relaxed);

			other.instance_count.store(0, std::memory_order_relaxed);
			other.dropped_instances.store(0, std::memory_order_relaxed);
		}

		return *this;
	}



	namespace {
		uint32_t find_split(const std::vector<RenderScene::LBVHNode>& nodes, uint32_t first, uint32_t last) {
			uint32_t first_code = nodes[first].morton_code;
			uint32_t last_code = nodes[last].morton_code;

			if (first_code == last_code) {
				return (first + last) >> 1;
			}

			int common_prefix = std::countl_zero(first_code ^ last_code);

			uint32_t split = first;
			uint32_t step = last - first;

			do {
				step = (step + 1) >> 1;
				uint32_t new_split = split + step;

				if (new_split < last) {
					uint32_t split_code = nodes[new_split].morton_code;
					int split_prefix = std::countl_zero(first_code ^ split_code);
					if (split_prefix > common_prefix) {
						split = new_split;
					}
				}
			} while (step > 1);

			return split;
		}

		uint32_t generate_hierarchy(
			std::vector<RenderScene::BVHNode>& bvh_nodes,
			const std::vector<RenderScene::LBVHNode>& lbvh_nodes,
			const std::vector<bud::math::AABB>& world_aabbs,
			uint32_t first, uint32_t last)
		{
			if (first == last) {
				uint32_t node_idx = static_cast<uint32_t>(bvh_nodes.size());
				bvh_nodes.push_back({});
				auto& node = bvh_nodes.back();
				node.is_leaf = true;
				node.instance_index = lbvh_nodes[first].instance_index;
				node.aabb = world_aabbs[node.instance_index];
				return node_idx;
			}

			uint32_t split = find_split(lbvh_nodes, first, last);
			uint32_t left_idx = generate_hierarchy(bvh_nodes, lbvh_nodes, world_aabbs, first, split);
			uint32_t right_idx = generate_hierarchy(bvh_nodes, lbvh_nodes, world_aabbs, split + 1, last);

			uint32_t node_idx = static_cast<uint32_t>(bvh_nodes.size());
			bvh_nodes.push_back({});
			auto& node = bvh_nodes.back();
			node.is_leaf = false;
			node.left_child = left_idx;
			node.right_child = right_idx;
			node.aabb = bvh_nodes[left_idx].aabb;
			node.aabb.merge(bvh_nodes[right_idx].aabb);
			return node_idx;
		}
	}

	void RenderScene::build_culling_lbvh() {
		size_t count = size();
		if (count == 0) {
			lbvh_nodes.clear();
			bvh_nodes.clear();
			scene_bounds = bud::math::AABB();
			bvh_root = ~0u;
			return;
		}

		lbvh_nodes.clear();
		lbvh_nodes.reserve(count);

		// 1. Calculate global bounds
		scene_bounds = bud::math::AABB();
		for (size_t i = 0; i < count; ++i) {
			scene_bounds.merge(world_aabbs[i]);
		}

		// 2. Compute Morton code for each instance
		for (size_t i = 0; i < count; ++i) {
			uint32_t mc = bud::math::compute_morton_code(world_aabbs[i].center(), scene_bounds);
			lbvh_nodes.push_back({ static_cast<uint32_t>(i), mc });
		}

		// 3. Sort nodes by Morton code
		std::sort(lbvh_nodes.begin(), lbvh_nodes.end());

		// 4. Build Tree
		bvh_nodes.clear();
		bvh_nodes.reserve(count * 2 - 1);
		bvh_root = generate_hierarchy(bvh_nodes, lbvh_nodes, world_aabbs, 0, static_cast<uint32_t>(count - 1));
	}

	void RenderScene::build_culling_lbvh_parallel(bud::threading::TaskScheduler* task_scheduler) {
		size_t count = size();
		if (count == 0) {
			lbvh_nodes.clear();
			bvh_nodes.clear();
			scene_bounds = bud::math::AABB();
			bvh_root = ~0u;
			return;
		}

		if (!task_scheduler || count < 512) {
			build_culling_lbvh();
			return;
		}

		constexpr size_t LBVH_CHUNK_SIZE = 256;
		size_t chunk_count = (count + LBVH_CHUNK_SIZE - 1) / LBVH_CHUNK_SIZE;

		std::vector<bud::math::AABB> chunk_bounds(chunk_count);
		bud::threading::Counter bounds_counter;
		task_scheduler->ParallelFor(count, LBVH_CHUNK_SIZE,
			[&](size_t start, size_t end_exclusive) {
				bud::math::AABB local_bounds;
				for (size_t i = start; i < end_exclusive; ++i) {
					local_bounds.merge(world_aabbs[i]);
				}
				chunk_bounds[start / LBVH_CHUNK_SIZE] = local_bounds;
			},
			&bounds_counter
		);
		task_scheduler->wait_for_counter(bounds_counter);

		scene_bounds = bud::math::AABB();
		for (const auto& chunk_bound : chunk_bounds) {
			scene_bounds.merge(chunk_bound);
		}

		lbvh_nodes.clear();
		lbvh_nodes.resize(count);

		bud::threading::Counter morton_counter;
		task_scheduler->ParallelFor(count, LBVH_CHUNK_SIZE,
			[&](size_t start, size_t end_exclusive) {
				for (size_t i = start; i < end_exclusive; ++i) {
					lbvh_nodes[i] = {
						static_cast<uint32_t>(i),
						bud::math::compute_morton_code(world_aabbs[i].center(), scene_bounds)
					};
				}
			},
			&morton_counter
		);
		task_scheduler->wait_for_counter(morton_counter);

		std::sort(lbvh_nodes.begin(), lbvh_nodes.end());

		bvh_nodes.clear();
		bvh_nodes.reserve(count * 2 - 1);
		bvh_root = generate_hierarchy(bvh_nodes, lbvh_nodes, world_aabbs, 0, static_cast<uint32_t>(count - 1));
	}

	void RenderScene::cull_frustum(const bud::math::Frustum& frustum, std::vector<uint32_t>& out_indices) const {
		if (bvh_root == ~0u) {
			// Fallback or empty
			size_t count = size();
			for (size_t i = 0; i < count; ++i) {
				if (bud::math::intersect_aabb_frustum(world_aabbs[i], frustum)) {
					out_indices.push_back(static_cast<uint32_t>(i));
				}
			}
			return;
		}

		std::vector<uint32_t> stack;
		stack.reserve(64);
		stack.push_back(bvh_root);

		while (!stack.empty()) {
			uint32_t node_idx = stack.back();
			stack.pop_back();

			const auto& node = bvh_nodes[node_idx];
			if (bud::math::intersect_aabb_frustum(node.aabb, frustum)) {
				if (node.is_leaf) {
					out_indices.push_back(node.instance_index);
				} else {
					stack.push_back(node.left_child);
					stack.push_back(node.right_child);
				}
			}
		}
	}

	bool RenderScene::intersect_scene(const bud::math::AABB& aabb) const {
		if (bvh_root == ~0u) {
			size_t count = size();
			for (size_t i = 0; i < count; ++i) {
				if (world_aabbs[i].intersects(aabb)) return true;
			}
			return false;
		}

		std::vector<uint32_t> stack;
		stack.reserve(64);
		stack.push_back(bvh_root);

		while (!stack.empty()) {
			uint32_t node_idx = stack.back();
			stack.pop_back();

			const auto& node = bvh_nodes[node_idx];
			if (node.aabb.intersects(aabb)) {
				if (node.is_leaf) {
					// Need to check actual intersection since BVH AABB might be slightly larger or represent multiple items
					// But for LBVH, leaf is one instance.
					if (world_aabbs[node.instance_index].intersects(aabb)) return true;
				} else {
					stack.push_back(node.left_child);
					stack.push_back(node.right_child);
				}
			}
		}

		return false;
	}
}
