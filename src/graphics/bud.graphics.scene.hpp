#pragma once

#include <vector>
#include <atomic>
#include <cstdint>

#include "src/core/bud.math.hpp"

namespace bud::graphics {

	// 这一帧所有可见物体的“扁平化”数据快照
	// SoA (Structure of Arrays) accelerate Cache effeciency and multi-threading processing
	struct RenderScene {
		// 变换矩阵 (用于 Draw)
		std::vector<bud::math::mat4> world_matrices;

		std::vector<bud::math::AABB> world_aabbs;

		// 资源索引 (指向 Renderer::meshes 等资源池)
		std::vector<uint32_t> mesh_indices;
		std::vector<uint32_t> material_indices;

		// [新增] 标志位 (Bit 0 = IsStatic, Bit 1 = CastShadow ...)
		std::vector<uint8_t> flags;

		// --- 计数器 ---
		// 当前帧的实例数量 (原子操作，支持多线程并行添加)
		std::atomic<size_t> instance_count{ 0 };

		// 1. 默认构造函数 (resize 需要)
		RenderScene() = default;

		// 2. 移动构造函数 (Move Constructor)
		RenderScene(RenderScene&& other) noexcept;

		// 3. 移动赋值运算符 (Move Assignment)
		RenderScene& operator=(RenderScene&& other) noexcept;

		// 4. 显式删除拷贝，防止意外发生 (Atomic 本身也是不可拷贝的)
		RenderScene(const RenderScene&) = delete;
		RenderScene& operator=(const RenderScene&) = delete;


		// 每一帧开始前调用：重置计数器，并根据预估数量预分配内存
		// 注意：不释放内存 (capacity 保持不变)，避免反复 malloc/free
		void reset(size_t estimated_capacity);

		// 线程安全地“申请”一个槽位并填入数据
		// 返回分配到的 index
		inline void add_instance(const bud::math::mat4& transform, const bud::math::AABB& aabb, uint32_t mesh_index, uint32_t material_index, bool is_static) {
			// 1. 原子获取索引 (相当于申请了一块内存)
			size_t idx = instance_count.fetch_add(1, std::memory_order_relaxed);

			// 安全检查 (生产环境可以去掉或者用 assert)
			if (idx >= world_matrices.size()) [[unlikely]] {
				// 严重错误：预分配不足。在 Fiber 环境下扩容是不安全的。
				// 简单的处理是丢弃，或者由主线程负责扩容
				return;
			}

			// 2. 并行写入 (无锁，因为每个线程操作不同的 idx)
			world_matrices[idx] = transform;
			world_aabbs[idx] = aabb;
			mesh_indices[idx] = mesh_index;
			material_indices[idx] = material_index;

			flags[idx] = is_static ? 1 : 0;
		}

		// 获取当前有效数量
		inline size_t size() const;
	};
}
