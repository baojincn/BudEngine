#include "src/graphics/bud.graphics.gpu_scene.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

// Compile-time layout validation for C++ / GLSL shared structs
#include "src/core/bud.layouts.hpp"

namespace bud::graphics {

	static inline uint32_t next_power_of_two(uint32_t v) {
		if (v == 0) return 1;
		return std::bit_ceil(v);
	}

	void GPUScene::init(RHI* rhi, uint32_t inflight_frame_count) {
		rhi_ptr = rhi;
		frame_resources.resize(inflight_frame_count);

		if (!rhi || geometry_pool.initialized) {
			return;
		}

		geometry_pool.vertex_buffer = rhi->create_gpu_buffer(GeometryPool::vertex_pool_size, ResourceState::VertexBuffer);
		geometry_pool.index_buffer = rhi->create_gpu_buffer(GeometryPool::index_pool_size, ResourceState::IndexBuffer);
		geometry_pool.initialized = geometry_pool.vertex_buffer.is_valid() && geometry_pool.index_buffer.is_valid();

		if (!page_pool.initialized) {
			page_pool.page_pool_buffer = rhi->create_gpu_buffer(PagePool::page_pool_size, ResourceState::UnorderedAccess);
			page_pool.initialized = page_pool.page_pool_buffer.is_valid();
			if (page_pool.initialized) {
				std::lock_guard lock(page_pool.mutex);
				page_pool.free_slots.reserve(PagePool::max_pages);
				for (int32_t i = PagePool::max_pages - 1; i >= 0; --i)
					page_pool.free_slots.push_back(i);
			}
		}

		if (!page_table_buffer.is_valid()) {
			page_table_buffer = rhi->create_upload_buffer(static_cast<uint64_t>(max_page_table_entries) * 12);
		}
		
		if (!vg_pool.group_buffer.is_valid()) {
			// 1024K groups max = 36MB. CPU 写入层次结构，需要 PersistentMapped
			vg_pool.group_buffer = rhi->create_upload_buffer(1024ull * 1024 * 36);
			vg_pool.next_group = 0;
			vg_pool.next_virtual_page = 0;
		}

		if (auto* buf = rhi->get_buffer(page_table_buffer); buf && buf->mapped_ptr) {
			std::memset(buf->mapped_ptr, 0, max_page_table_entries * sizeof(PageTableEntry));
		}

		if (!materials_buffer.is_valid()) {
			materials_buffer = rhi->create_upload_buffer(static_cast<uint64_t>(max_materials) * sizeof(GPUMaterialData));
			if (auto* buf = rhi->get_buffer(materials_buffer); buf && buf->mapped_ptr) {
				std::memset(buf->mapped_ptr, 0, max_materials * sizeof(GPUMaterialData));
			}
			// Slot 0 is default fallback material
			GPUMaterialData default_mat;
			default_mat.base_color_factor = glm::vec4(0.8f, 0.8f, 0.8f, 1.0f);
			default_mat.metallic_factor = 0.0f;
			default_mat.roughness_factor = 0.5f;
			default_mat.alpha_cutoff = 0.5f;
			default_mat.alpha_mode = 0;
			register_material(default_mat);
		}
	}

	void GPUScene::shutdown(RHI* rhi) {
		rhi_ptr = nullptr;
		if (rhi && geometry_pool.initialized) {
			if (geometry_pool.vertex_buffer.is_valid())
				rhi->destroy_buffer(geometry_pool.vertex_buffer);
			if (geometry_pool.index_buffer.is_valid())
				rhi->destroy_buffer(geometry_pool.index_buffer);
		}

		geometry_pool.vertex_buffer = {};
		geometry_pool.index_buffer = {};
		geometry_pool.next_vertex.store(0, std::memory_order_relaxed);
		geometry_pool.next_index.store(0, std::memory_order_relaxed);
		geometry_pool.initialized = false;

		if (rhi && page_pool.initialized) {
			if (page_pool.page_pool_buffer.is_valid())
				rhi->destroy_buffer(page_pool.page_pool_buffer);
		}
		page_pool.page_pool_buffer = {};
		{
			std::lock_guard lock(page_pool.mutex);
			page_pool.free_slots.clear();
		}
		page_pool.initialized = false;

		if (rhi && page_table_buffer.is_valid()) {
			rhi->destroy_buffer(page_table_buffer);
		}
		page_table_buffer = {};

		if (rhi && materials_buffer.is_valid()) {
			rhi->destroy_buffer(materials_buffer);
		}
		materials_buffer = {};
		{
			std::lock_guard lock(materials_mutex);
			cpu_materials.clear();
		}
		
		if (rhi && vg_pool.group_buffer.is_valid()) {
			rhi->destroy_buffer(vg_pool.group_buffer);
		}
		vg_pool.group_buffer = {};

		if (rhi) {
			for (auto& frame_resource : frame_resources) {
				if (frame_resource.instance_data.is_valid()) rhi->destroy_buffer(frame_resource.instance_data);
				if (frame_resource.indirect_instance.is_valid()) rhi->destroy_buffer(frame_resource.indirect_instance);
				if (frame_resource.indirect_draw.is_valid()) rhi->destroy_buffer(frame_resource.indirect_draw);
				if (frame_resource.stats_readback.is_valid()) rhi->destroy_buffer(frame_resource.stats_readback);
				if (frame_resource.page_request_buffer.is_valid()) rhi->destroy_buffer(frame_resource.page_request_buffer);
				if (frame_resource.page_request_readback.is_valid()) rhi->destroy_buffer(frame_resource.page_request_readback);
				if (frame_resource.csm_static_indirect_draw.is_valid()) rhi->destroy_buffer(frame_resource.csm_static_indirect_draw);
				if (frame_resource.csm_instance_data.is_valid()) rhi->destroy_buffer(frame_resource.csm_instance_data);
				if (frame_resource.visible_pages.is_valid()) rhi->destroy_buffer(frame_resource.visible_pages);
				if (frame_resource.visible_pages_readback.is_valid()) rhi->destroy_buffer(frame_resource.visible_pages_readback);
				for (auto& csm_vp : frame_resource.csm_visible_pages) {
					if (csm_vp.is_valid()) rhi->destroy_buffer(csm_vp);
				}
				if (frame_resource.visible_clusters.is_valid()) rhi->destroy_buffer(frame_resource.visible_clusters);
				if (frame_resource.dynamic_instances.is_valid()) rhi->destroy_buffer(frame_resource.dynamic_instances);
				frame_resource = {};
			}
		}

		mesh_geometries.clear();
		frame_resources.clear();
	}

	uint32_t GPUScene::register_material(const GPUMaterialData& material) {
		std::lock_guard lock(materials_mutex);
		uint32_t id = static_cast<uint32_t>(cpu_materials.size());
		if (id >= max_materials) {
			return 0; // fallback to default
		}
		cpu_materials.push_back(material);
		if (rhi_ptr) {
			if (auto* buf = rhi_ptr->get_buffer(materials_buffer); buf && buf->mapped_ptr) {
				auto* dest = reinterpret_cast<GPUMaterialData*>(buf->mapped_ptr);
				dest[id] = material;
			}
		}
		return id;
	}

	void GPUScene::update_material(uint32_t material_id, const GPUMaterialData& material) {
		std::lock_guard lock(materials_mutex);
		if (material_id >= cpu_materials.size()) {
			return;
		}
		cpu_materials[material_id] = material;
		if (rhi_ptr) {
			if (auto* buf = rhi_ptr->get_buffer(materials_buffer); buf && buf->mapped_ptr) {
				auto* dest = reinterpret_cast<GPUMaterialData*>(buf->mapped_ptr);
				dest[material_id] = material;
			}
		}
	}

	GPUScene::GeometryPool& GPUScene::get_geometry_pool() {
		return geometry_pool;
	}

	const GPUScene::GeometryPool& GPUScene::get_geometry_pool() const {
		return geometry_pool;
	}

	BufferHandle GPUScene::get_vertex_buffer() const {
		return geometry_pool.vertex_buffer;
	}

	BufferHandle GPUScene::get_index_buffer() const {
		return geometry_pool.index_buffer;
	}

	void GPUScene::set_mesh_geometry(uint32_t mesh_id, uint32_t first_index, int32_t vertex_offset) {
		if (mesh_geometries.size() <= mesh_id) {
			mesh_geometries.resize(mesh_id + 1);
		}

		mesh_geometries[mesh_id].first_index = first_index;
		mesh_geometries[mesh_id].vertex_offset = vertex_offset;
	}

	const GPUScene::MeshGeometry& GPUScene::get_mesh_geometry(uint32_t mesh_id) const {
		return mesh_geometries.at(mesh_id);
	}

	GPUScene::FrameResources& GPUScene::get_frame_resources(uint32_t frame_index) {
		return frame_resources.at(frame_index);
	}

	const GPUScene::FrameResources& GPUScene::get_frame_resources(uint32_t frame_index) const {
		return frame_resources.at(frame_index);
	}

	void GPUScene::ensure_frame_resources(RHI* rhi,
		uint32_t frame_index,
		uint32_t required_instance_count,
		uint32_t required_draw_count,
		uint32_t required_scene_instance_count,
		uint64_t instance_data_stride,
		uint64_t indirect_instance_stride,
		uint64_t indirect_draw_stride,
		uint64_t stats_buffer_size)
	{
		if (!rhi || frame_index >= frame_resources.size()) {
			return;
		}

		auto& frame_resource = frame_resources[frame_index];

		uint32_t desired_instance_capacity = std::max(64u, next_power_of_two(required_instance_count));
		uint32_t desired_indirect_capacity = std::max(64u, next_power_of_two(required_draw_count));
		uint32_t desired_scene_capacity = std::max(64u, next_power_of_two(required_scene_instance_count));
		uint32_t desired_cluster_capacity = 65536u; // Capacity for GPU-driven Virtual Geometry clusters

		// The indirect draw buffer must be large enough to hold commands
		// emitted by cluster_cull.comp (one per visible cluster, up to
		// desired_cluster_capacity).
		desired_indirect_capacity = std::max(desired_indirect_capacity, desired_cluster_capacity);

		if (frame_resource.instance_capacity < desired_instance_capacity || !frame_resource.instance_data.is_valid()) {
			if (frame_resource.instance_data.is_valid())
				rhi->destroy_buffer(frame_resource.instance_data);
			// CPU 每帧直接 memcpy 写入，需要 PersistentMapped（host-visible）以保证 mapped_ptr 有效
			frame_resource.instance_data = rhi->create_upload_buffer(static_cast<uint64_t>(desired_instance_capacity) * instance_data_stride);
			frame_resource.instance_capacity = desired_instance_capacity;
		}

		if (frame_resource.indirect_capacity < desired_indirect_capacity || !frame_resource.indirect_instance.is_valid()) {
			if (frame_resource.indirect_instance.is_valid())
				rhi->destroy_buffer(frame_resource.indirect_instance);
			// CPU 每帧 memcpy 写入 DrawData，需要 PersistentMapped
			frame_resource.indirect_instance = rhi->create_upload_buffer(static_cast<uint64_t>(desired_indirect_capacity) * indirect_instance_stride);
		}

		if (frame_resource.indirect_capacity < desired_indirect_capacity || !frame_resource.indirect_draw.is_valid()) {
			if (frame_resource.indirect_draw.is_valid())
				rhi->destroy_buffer(frame_resource.indirect_draw);
			frame_resource.indirect_draw = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_indirect_capacity) * indirect_draw_stride, ResourceState::IndirectArgument);
		}

		// Full-scene CSM indirect draw buffer for GPU-driven shadow culling.
		// Size: cascade_count * scene_capacity entries.
		if (!frame_resource.csm_indirect_draw.is_valid() || frame_resource.csm_indirect_capacity < desired_scene_capacity) {
			if (frame_resource.csm_indirect_draw.is_valid())
				rhi->destroy_buffer(frame_resource.csm_indirect_draw);
			frame_resource.csm_indirect_draw = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_scene_capacity) * 4 * indirect_draw_stride, ResourceState::IndirectArgument);
				frame_resource.csm_indirect_capacity = static_cast<uint32_t>(desired_scene_capacity);
			}

			// Static-only indirect commands for the CSM static cache update.
			if (!frame_resource.csm_static_indirect_draw.is_valid() || frame_resource.csm_indirect_capacity < desired_scene_capacity) {
				if (frame_resource.csm_static_indirect_draw.is_valid())
					rhi->destroy_buffer(frame_resource.csm_static_indirect_draw);
				frame_resource.csm_static_indirect_draw = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_scene_capacity) * 4 * indirect_draw_stride, ResourceState::IndirectArgument);
			}

			// Full-scene DrawData for CSM cull (covers out-of-view casters).
			if (!frame_resource.csm_instance_data.is_valid() || frame_resource.csm_instance_capacity < desired_scene_capacity) {
				if (frame_resource.csm_instance_data.is_valid())
					rhi->destroy_buffer(frame_resource.csm_instance_data);
				// MAX_CASCADES is 4, we allocate 4x capacity. CPU 每帧写入，需要 PersistentMapped
				frame_resource.csm_instance_data = rhi->create_upload_buffer(static_cast<uint64_t>(desired_scene_capacity) * 4 * indirect_instance_stride);
				frame_resource.csm_instance_capacity = desired_scene_capacity;
			}

			// Full-scene InstanceData (model+material) matching the reordered
			// full-scene DrawData, used by shadow.vert during CSM GPU draws.
			if (!frame_resource.csm_instance_models.is_valid() || frame_resource.csm_instance_models_capacity < desired_scene_capacity) {
				if (frame_resource.csm_instance_models.is_valid())
					rhi->destroy_buffer(frame_resource.csm_instance_models);
				// CPU 每帧写入，需要 PersistentMapped
				frame_resource.csm_instance_models = rhi->create_upload_buffer(static_cast<uint64_t>(desired_scene_capacity) * instance_data_stride);
				frame_resource.csm_instance_models_capacity = desired_scene_capacity;
			}

			if (!frame_resource.stats_readback.is_valid()) {
				frame_resource.stats_readback = rhi->create_readback_buffer(stats_buffer_size);
			}

			if (!frame_resource.page_request_buffer.is_valid()) {
				constexpr uint32_t page_request_size = 16384; // 12 bytes header + ~4093 page requests
				frame_resource.page_request_buffer = rhi->create_gpu_buffer(page_request_size, ResourceState::UnorderedAccess);
				frame_resource.page_request_readback = rhi->create_readback_buffer(page_request_size);
				if (auto* buf = rhi->get_buffer(frame_resource.page_request_readback); buf && buf->mapped_ptr)
					std::memset(buf->mapped_ptr, 0, page_request_size);
			}

			if (!frame_resource.visible_pages.is_valid()) {
				constexpr uint32_t max_visible_pages = 65536;
				uint64_t vp_size = 4 + static_cast<uint64_t>(max_visible_pages) * sizeof(uint32_t);
				frame_resource.visible_pages = rhi->create_gpu_buffer(vp_size, ResourceState::UnorderedAccess);
				frame_resource.visible_pages_readback = rhi->create_readback_buffer(vp_size);
				frame_resource.visible_page_capacity = max_visible_pages;
			}

			for (size_t c_idx = 0; c_idx < frame_resource.csm_visible_pages.size(); ++c_idx) {
				if (!frame_resource.csm_visible_pages[c_idx].is_valid()) {
					constexpr uint32_t max_visible_pages = 65536;
					uint64_t vp_size = 4 + static_cast<uint64_t>(max_visible_pages) * sizeof(uint32_t);
					frame_resource.csm_visible_pages[c_idx] = rhi->create_gpu_buffer(vp_size, ResourceState::UnorderedAccess);
				}
			}

			if (!frame_resource.visible_clusters.is_valid() || frame_resource.visible_cluster_capacity < desired_cluster_capacity) {
				if (frame_resource.visible_clusters.is_valid())
					rhi->destroy_buffer(frame_resource.visible_clusters);
				// 1 uint for count + N * 12 bytes (VisibleCluster struct is 12 bytes: 3 uint32s)
				uint64_t vc_size = 4 + static_cast<uint64_t>(desired_cluster_capacity) * 12;
				frame_resource.visible_clusters = rhi->create_gpu_buffer(vc_size, ResourceState::UnorderedAccess);
			}

			if (!frame_resource.dynamic_instances.is_valid() || frame_resource.visible_cluster_capacity < desired_cluster_capacity) {
				if (frame_resource.dynamic_instances.is_valid())
					rhi->destroy_buffer(frame_resource.dynamic_instances);
				// 1 uint for count + N * instance_data_stride
				uint64_t di_size = 4 + static_cast<uint64_t>(desired_cluster_capacity) * instance_data_stride;
				frame_resource.dynamic_instances = rhi->create_gpu_buffer(di_size, ResourceState::UnorderedAccess);
			}
			frame_resource.visible_cluster_capacity = desired_cluster_capacity;

			frame_resource.indirect_capacity = desired_indirect_capacity;
	}

	GPUScene::PagePool& GPUScene::get_page_pool() { return page_pool; }
	const GPUScene::PagePool& GPUScene::get_page_pool() const { return page_pool; }
	BufferHandle GPUScene::get_page_pool_buffer() const { return page_pool.page_pool_buffer; }
	BufferHandle GPUScene::get_page_table_buffer() const { return page_table_buffer; }

	void GPUScene::update_page_table_entry(uint32_t page_index, uint32_t pool_offset, uint32_t valid) {
		if (!page_table_buffer.is_valid() || page_index >= max_page_table_entries || !rhi_ptr)
			return;
		auto* buf = rhi_ptr->get_buffer(page_table_buffer);
		if (!buf || !buf->mapped_ptr) return;
		auto* entries = static_cast<PageTableEntry*>(buf->mapped_ptr);
		// Write pool_offset BEFORE valid so that the GPU (which reads valid first)
		// never sees valid=1 with a stale pool_offset.
		// HOST_COHERENT memory guarantees CPU writes are visible to GPU in program order.
		entries[page_index].pool_offset = pool_offset;
		std::atomic_signal_fence(std::memory_order_release);
		entries[page_index].valid = valid;
	}

	uint32_t GPUScene::PagePool::allocate_page() {
		std::lock_guard lock(mutex);
		if (free_slots.empty()) return ~0u;
		uint32_t slot = free_slots.back();
		free_slots.pop_back();
		return slot;
	}

	void GPUScene::PagePool::free_page(uint32_t page_index) {
		std::lock_guard lock(mutex);
		free_slots.push_back(page_index);
	}
}
