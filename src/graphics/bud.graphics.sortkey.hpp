#pragma once
#include <cstdint>
#include <algorithm>

namespace bud::graphics {

	// 布局: [ Layer(4) | Pipeline(10) | Material(18) | Mesh(14) | Depth(18) ]
	// 总计 64 bits
	struct DrawKey {
		uint64_t value = 0;

		// --- 编码器 (Encoder) ---

		// Opaque Key: 
		// 优先级: Layer > Pipeline > Material > Mesh > Depth
		// 这样排序后：
		// 1. 相同材质的物体聚在一起 (减少 State 切换 - SetPass Calls)
		// 2. 在材质内部，相同 Mesh 的物体聚在一起 (允许 Instancing 合批)
		// 3. 最后才是深度 (减少 Overdraw，但被迫为合批让路，牺牲一点点深度排序精度)

		static inline uint64_t generate_opaque(uint8_t layer, uint16_t pipeline_id, uint32_t material_id, uint32_t mesh_id, uint32_t depth_18bit) {
			uint64_t key = 0;

			// 1. Layer (4 bits) [60-63]
			key |= (uint64_t)(layer & 0xF) << 60;

			// 2. Pipeline (10 bits) [50-59] - 支持 1024 种 Shader
			key |= (uint64_t)(pipeline_id & 0x3FF) << 50;

			// 3. Material (18 bits) [32-49] - 支持 26万 种材质
			key |= (uint64_t)(material_id & 0x3FFFF) << 32;

			// 4. Mesh (14 bits) [18-31] - 支持 1.6万 种 Mesh 用于 Instancing
			key |= (uint64_t)(mesh_id & 0x3FFF) << 18;

			// 5. Depth (18 bits) [0-17] - 归一化深度 (0~26万)
			key |= (uint64_t)(depth_18bit & 0x3FFFF);

			return key;
		}

		// Translucent Key: Back-to-Front (Layer | Depth)
		// 透明物体通常无法 Instancing，因为必须严格按深度排序以保证混合正确
		static inline uint64_t generate_translucent(uint8_t layer, float depth_from_camera) {
			uint32_t depth_int = *reinterpret_cast<uint32_t*>(&depth_from_camera);
			uint32_t inv_depth = ~depth_int; // 翻转浮点位，实现从大到小排序

			uint64_t key = 0;
			key |= (uint64_t)(layer & 0xF) << 60;
			key |= (uint64_t)inv_depth << 28; // 使用高位 32bits 存深度
			return key;
		}
	};

	struct SortItem {
		uint64_t key;
		uint32_t entity_index;
	};
}
