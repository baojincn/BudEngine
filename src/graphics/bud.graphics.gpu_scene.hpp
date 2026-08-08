#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "src/graphics/bud.graphics.memory.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics {

	class GPUScene {
	public:
		struct MeshGeometry {
			uint32_t first_index = 0;
			int32_t vertex_offset = 0;
		};

		struct GeometryPool {
			static constexpr uint64_t kVertexPoolSize = 256ull * 1024 * 1024;
			static constexpr uint64_t kIndexPoolSize = 128ull * 1024 * 1024;

			BufferHandle vertex_buffer;
			BufferHandle index_buffer;
			std::atomic<uint32_t> next_vertex{ 0 };
			std::atomic<uint32_t> next_index{ 0 };
			bool initialized = false;
		};

		struct PagePool {
			static constexpr uint64_t kPagePoolSize = 512ull * 1024 * 1024;
			// 131040 is an exact multiple of 48 (sizeof asset::Vertex), 24, 16, and 4
			static constexpr uint32_t kPageSize = 131040;
			static constexpr uint32_t kMaxPages = kPagePoolSize / kPageSize;

			BufferHandle page_pool_buffer;
			std::vector<uint32_t> free_slots;
			std::mutex mutex;
			bool initialized = false;

			uint32_t allocate_page();
			void free_page(uint32_t page_index);
			uint32_t get_page_offset(uint32_t page_index) const { return page_index * kPageSize; }
		};

		struct PageTableEntry {
			uint32_t valid;
			uint32_t padding;
			uint32_t pool_offset;
		};
		static constexpr uint32_t kMaxPageTableEntries = 4096;

		struct FrameResources {
			BufferHandle instance_data;
			BufferHandle indirect_instance;
			BufferHandle indirect_draw;
			BufferHandle stats_readback;
			BufferHandle meshlet_frustum_stats;
			BufferHandle meshlet_hiz_stats;
			BufferHandle meshlet_visibility;
			BufferHandle meshlet_hiz_visibility;
			BufferHandle csm_indirect_draw;
			// Static-only indirect commands (one range per cascade) written by
			// csm_cull.comp with static_only=1; used by the CSM static cache
			// update pass so dynamic objects are never drawn into the cache.
			BufferHandle csm_static_indirect_draw;
			// Full-scene DrawData (every scene instance, page/non-page reordered)
			// consumed by csm_cull.comp so the CSM pass covers casters outside
			// the main camera view without per-instance CPU draw calls.
			BufferHandle csm_instance_data;
			// Full-scene InstanceData (model + material) in the same reordered
			// order, bound to the shadow pipeline's instance binding (set 0,
			// binding 3) while the CSM GPU draws are recorded.
			BufferHandle csm_instance_models;
			uint32_t instance_capacity = 0;
			uint32_t csm_indirect_capacity = 0;
			uint32_t csm_instance_capacity = 0;
			uint32_t csm_instance_models_capacity = 0;
			uint32_t indirect_capacity = 0;
			uint32_t meshlet_visibility_capacity = 0;
		};

		GPUScene() = default;
		~GPUScene() = default;

		GPUScene(const GPUScene&) = delete;
		GPUScene& operator=(const GPUScene&) = delete;

		GPUScene(GPUScene&&) = delete;
		GPUScene& operator=(GPUScene&&) = delete;

		void init(RHI* rhi, uint32_t inflight_frame_count);
		void shutdown(RHI* rhi);

		GeometryPool& geometry_pool();
		const GeometryPool& geometry_pool() const;

		BufferHandle get_vertex_buffer() const;
		BufferHandle get_index_buffer() const;

		PagePool& get_page_pool();
		const PagePool& get_page_pool() const;
		BufferHandle get_page_pool_buffer() const;
		BufferHandle get_page_table_buffer() const;
		void update_page_table_entry(uint32_t page_index, uint32_t pool_offset);

		void set_mesh_geometry(uint32_t mesh_id, uint32_t first_index, int32_t vertex_offset);
		const MeshGeometry& mesh_geometry(uint32_t mesh_id) const;

		FrameResources& frame_resources(uint32_t frame_index);
		const FrameResources& frame_resources(uint32_t frame_index) const;

		void ensure_frame_resources(RHI* rhi,
			uint32_t frame_index,
			uint32_t required_instance_count,
			uint32_t required_draw_count,
			uint32_t required_scene_instance_count,
			uint32_t required_meshlet_count,
			uint64_t instance_data_stride,
			uint64_t indirect_instance_stride,
			uint64_t indirect_draw_stride,
			uint64_t stats_buffer_size,
			bool enable_gpu_driven,
			bool enable_meshlets);

	private:
		GeometryPool geometry_pool_;
		PagePool page_pool_;
		BufferHandle page_table_buffer_;
		std::vector<MeshGeometry> mesh_geometry_;
		std::vector<FrameResources> frame_resources_;
	};
}
