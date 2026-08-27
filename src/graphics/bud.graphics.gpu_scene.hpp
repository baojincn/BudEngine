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
			static constexpr uint64_t vertex_pool_size = 256ull * 1024 * 1024;
			static constexpr uint64_t index_pool_size = 128ull * 1024 * 1024;

			BufferHandle vertex_buffer;
			BufferHandle index_buffer;
			std::atomic<uint32_t> next_vertex{ 0 };
			std::atomic<uint32_t> next_index{ 0 };
			bool initialized = false;
		};

		struct PagePool {
			// 1 GB holds ~8000 fixed 128 KB slots: enough for the resident set of
			// a full San-Miguel-class scene (~5100 pages) in the CPU-driven path.
			static constexpr uint64_t page_pool_size = 1024ull * 1024 * 1024;
			// 131040 is an exact multiple of 48 (sizeof asset::Vertex), 24, 16, and 4.
			// Fixed 128 KB slots keep page-pool memory management simple. The tool
			// constrains each .budmesh page so its CPU-decoded legacy layout (48B
			// vertices + u32 indices) fits this slot.
			static constexpr uint32_t page_size = 128 * 1024; // 131072 bytes (UE5 Nanite 128 KB exact)
			static constexpr uint32_t max_pages = page_pool_size / page_size;

			BufferHandle page_pool_buffer;
			std::vector<uint32_t> free_slots;
			std::mutex mutex;
			bool initialized = false;

			uint32_t allocate_page();
			void free_page(uint32_t page_index);
			uint32_t get_page_offset(uint32_t page_index) const { return page_index * page_size; }
		};

		struct PageTableEntry {
			uint32_t valid;
			uint32_t padding;
			uint32_t pool_offset;
		};
		static constexpr uint32_t max_page_table_entries = 65536;
		
		struct VirtualGeometryPool {
			std::atomic<uint32_t> next_virtual_page{0};
			std::atomic<uint32_t> next_group{0};
			BufferHandle group_buffer; // Stores HierarchyCluster for all assets
			
			uint32_t allocate_virtual_pages(uint32_t count) { return next_virtual_page.fetch_add(count); }
			uint32_t allocate_groups(uint32_t count) {
		uint32_t start = next_group.fetch_add(count);
		// 1024K groups max = 36MB buffer
		constexpr uint32_t max_groups = 1024u * 1024u;
		if (start + count > max_groups) {
			bud::eprint("[GPUScene] Group buffer overflow! {} + {} > {}", start, count, max_groups);
			return 0;
		}
		return start;
	}
		};

		struct FrameResources {
			BufferHandle instance_data;
			BufferHandle indirect_instance;
			BufferHandle indirect_draw;
			BufferHandle stats_readback;
			BufferHandle page_request_buffer;         // GPU device-local: written by hierarchy_traversal.comp
			BufferHandle page_request_readback;       // Host-visible: copied from page_request_buffer each frame
			BufferHandle visible_pages;       // Phase 1 GPU-driven (hierarchy_traversal -> page_emit)
			BufferHandle visible_clusters;    // Phase 2 GPU-driven (page_emit -> cluster_cull)
			BufferHandle dynamic_instances;   // Phase 2 GPU-driven dynamic instances per cluster
			uint64_t submit_timeline_value = 0;
			bool requests_processed = true;
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
			uint32_t page_request_capacity = 0;
			uint32_t visible_page_capacity = 0;
			uint32_t visible_cluster_capacity = 0;
		};

		GPUScene() = default;
		~GPUScene() = default;

		GPUScene(const GPUScene&) = delete;
		GPUScene& operator=(const GPUScene&) = delete;

		GPUScene(GPUScene&&) = delete;
		GPUScene& operator=(GPUScene&&) = delete;

		void init(RHI* rhi, uint32_t inflight_frame_count);
		void shutdown(RHI* rhi);

		GeometryPool& get_geometry_pool();
		const GeometryPool& get_geometry_pool() const;

		BufferHandle get_vertex_buffer() const;
		BufferHandle get_index_buffer() const;

		PagePool& get_page_pool();
		const PagePool& get_page_pool() const;
		VirtualGeometryPool& get_vg_pool() { return vg_pool; }
		const VirtualGeometryPool& get_vg_pool() const { return vg_pool; }
		BufferHandle get_page_pool_buffer() const;
		BufferHandle get_page_table_buffer() const;
		void update_page_table_entry(uint32_t page_index, uint32_t pool_offset, uint32_t valid = 1);

		static constexpr uint32_t max_materials = 4096;
		BufferHandle get_materials_buffer() const { return materials_buffer; }
		uint32_t register_material(const GPUMaterialData& material);
		void update_material(uint32_t material_id, const GPUMaterialData& material);
		const std::vector<GPUMaterialData>& get_cpu_materials() const { return cpu_materials; }

		void set_mesh_geometry(uint32_t mesh_id, uint32_t first_index, int32_t vertex_offset);
		const MeshGeometry& get_mesh_geometry(uint32_t mesh_id) const;

		FrameResources& get_frame_resources(uint32_t frame_index);
		const FrameResources& get_frame_resources(uint32_t frame_index) const;

		std::vector<FrameResources>& get_frame_resources() { return frame_resources; }
		const std::vector<FrameResources>& get_frame_resources() const { return frame_resources; }

		void ensure_frame_resources(RHI* rhi,
			uint32_t frame_index,
			uint32_t required_instance_count,
			uint32_t required_draw_count,
			uint32_t required_scene_instance_count,
			uint64_t instance_data_stride,
			uint64_t indirect_instance_stride,
			uint64_t indirect_draw_stride,
			uint64_t stats_buffer_size);

	private:
		GeometryPool geometry_pool;
		PagePool page_pool;
		VirtualGeometryPool vg_pool;
		BufferHandle page_table_buffer;
		BufferHandle materials_buffer;
		std::vector<GPUMaterialData> cpu_materials;
		std::mutex materials_mutex;
		std::vector<MeshGeometry> mesh_geometries;
		std::vector<FrameResources> frame_resources;
		RHI* rhi_ptr = nullptr;
	};
}
