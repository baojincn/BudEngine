module;

#include <cstdint>
#include <string>

export module bud.graphics.types;

import bud.math;
import bud.graphics.defs;

export namespace bud::graphics {

	using CommandHandle = void*;

	export struct MemoryBlock {
		void* internal_handle = nullptr;
		uint64_t offset = 0;
		uint64_t size = 0;
		void* mapped_ptr = nullptr;
		bool is_valid() const { return internal_handle != nullptr; }
	};

	// 纹理基类
	export class Texture {
	public:
		virtual ~Texture() = default;

		uint32_t width = 0;
		uint32_t height = 0;
		TextureFormat format = TextureFormat::RGBA8_UNORM;
		uint32_t mips = 1;
		uint32_t array_layers = 1;

		size_t desc_hash = 0;
	};


	export struct CascadeData {
		bud::math::mat4 view_proj_matrix;
		float split_depth;
	};

	export struct RenderMesh {
		MemoryBlock vertex_buffer;
		MemoryBlock index_buffer;
		uint32_t index_count = 0;
		bud::math::AABB aabb; // [CSM]
		bud::math::BoundingSphere sphere; // [CSM]


		bool is_valid() const { return index_count > 0; }
	};


}
