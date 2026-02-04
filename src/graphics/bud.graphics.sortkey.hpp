#pragma once
#include <cstdint>

namespace bud::graphics {

	// 工业级 64-bit 排序键封装
	// 布局: [ Layer(4) | Pipeline(16) | Material(20) | Mesh(16) | Depth(8) ]
	// 总计 64 bits
	struct DrawKey {
		uint64_t value = 0;

		// 辅助联合体用于调试 (不要直接用它排序，Endianness 会坑死你)
		// 仅用于理解位布局
		/*
		union {
			uint64_t all;
			struct {
				uint64_t depth    : 8;  // 低位
				uint64_t mesh     : 16;
				uint64_t material : 20;
				uint64_t pipeline : 16;
				uint64_t layer    : 4;  // 高位
			} fields;
		};
		*/

		// --- 编码器 (Encoder) ---

		static inline uint64_t generate_opaque(uint8_t layer, uint16_t pipeline_id, uint32_t material_id, uint16_t mesh_id) {
			// 我们希望 state 切换越少越好，所以把 costly 的状态放在高位
			// 当我们对 uint64 排序时，高位相同的会聚在一起

			uint64_t key = 0;
			key |= (uint64_t)(layer & 0xF) << 60; // Top 4 bits
			key |= (uint64_t)(pipeline_id & 0xFFFF) << 44; // Next 16 bits
			key |= (uint64_t)(material_id & 0xFFFFF) << 24; // Next 20 bits (支持 100万个材质实例)
			key |= (uint64_t)(mesh_id & 0xFFFF) << 8;  // Next 16 bits
			// 最后 8 bits 留零，或者放粗略的深度做 Front-to-Back 优化
			return key;
		}

		static inline uint64_t generate_translucent(uint8_t layer, float depth_from_camera) {
			// 透明物体只关心层级和深度
			// 深度需要量化为整数 (Quantization)
			// 注意：透明物体是从远到近 (Back-to-Front)，所以深度大的 Key 应该小？
			// 不，通常是从远到近画。std::sort 是从小到大。
			// 假设 0 是近，MAX 是远。我们要先画 MAX，再画 0。
			// 所以我们需要 降序 排序，或者把深度取反。

			// 这里演示简单的量化：假设深度范围 0~5000
			// 我们把浮点深度转为 32位 整数。
			// 为了让远处的物体 (Depth 大) 排在前面 (std::sort 默认升序)，我们需要翻转深度值
			// 或者在排序 lambda 里写 >。这里我们按标准 Key 越小越先画来设计：

			// 策略：Key = Layer | (~Depth)
			// 这样深度大的物体，~Depth 小，会被排在前面。

			uint32_t depth_int = *reinterpret_cast<uint32_t*>(&depth_from_camera);
			// 浮点数黑魔法：对于正浮点数，可以直接当作整数比较大小
			// 取反操作：
			uint32_t inv_depth = ~depth_int;

			uint64_t key = 0;
			key |= (uint64_t)(layer & 0xF) << 60;
			key |= (uint64_t)inv_depth; // 低 32 位全部用于深度精度
			return key;
		}
	};

	// 排序项：Key + 指向数据的 Index
	struct SortItem {
		uint64_t key;
		uint32_t entity_index; // 回指 RenderScene 的索引
	};
}
