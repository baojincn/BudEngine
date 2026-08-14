#include "src/graphics/bud.graphics.gpu_scene.hpp"

#include <algorithm>

namespace bud::graphics {

	void GPUScene::init(RHI* rhi, uint32_t inflight_frame_count) {
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
			page_table_buffer = rhi->create_gpu_buffer(max_page_table_entries * sizeof(PageTableEntry), ResourceState::UnorderedAccess);
			if (page_table_buffer.mapped_ptr) {
				std::memset(page_table_buffer.mapped_ptr, 0, max_page_table_entries * sizeof(PageTableEntry));
			}
		}
	}

	void GPUScene::shutdown(RHI* rhi) {
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

		if (rhi) {
			for (auto& frame_resource : frame_resources) {
				if (frame_resource.instance_data.is_valid()) rhi->destroy_buffer(frame_resource.instance_data);
				if (frame_resource.indirect_instance.is_valid()) rhi->destroy_buffer(frame_resource.indirect_instance);
				if (frame_resource.indirect_draw.is_valid()) rhi->destroy_buffer(frame_resource.indirect_draw);
				if (frame_resource.stats_readback.is_valid()) rhi->destroy_buffer(frame_resource.stats_readback);
				if (frame_resource.meshlet_frustum_stats.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_frustum_stats);
				if (frame_resource.meshlet_hiz_stats.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_hiz_stats);
				if (frame_resource.meshlet_visibility.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_visibility);
				if (frame_resource.meshlet_hiz_visibility.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_hiz_visibility);
				if (frame_resource.csm_static_indirect_draw.is_valid()) rhi->destroy_buffer(frame_resource.csm_static_indirect_draw);
				if (frame_resource.csm_instance_data.is_valid()) rhi->destroy_buffer(frame_resource.csm_instance_data);
				if (frame_resource.csm_instance_models.is_valid()) rhi->destroy_buffer(frame_resource.csm_instance_models);
				frame_resource = {};
			}
		}

		mesh_geometries.clear();
		frame_resources.clear();
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
		uint32_t required_meshlet_count,
		uint64_t instance_data_stride,
		uint64_t indirect_instance_stride,
		uint64_t indirect_draw_stride,
		uint64_t stats_buffer_size,
		bool enable_gpu_driven,
		bool enable_meshlets)
	{
		if (!rhi || frame_index >= frame_resources.size()) {
			return;
		}

		auto& frame_resource = frame_resources[frame_index];
		const uint32_t desired_instance_capacity = std::max(required_instance_count + 1024u, 1024u);
		// Generous margins: page-backed meshes register asynchronously, so the
		// draw/instance counts passed this frame can lag behind the actual counts
		// recorded later in the same frame (a +1024 margin overflowed on a
		// San-Miguel-class scene where ~5k pages stream in). 2x/4x + 8192 keeps
		// the indirect buffers safe while the scene set settles.
		const uint32_t desired_indirect_capacity = std::max(required_draw_count * 2u + 8192u, 8192u);
		const uint32_t desired_scene_capacity = std::max(required_scene_instance_count * 4u + 8192u, 8192u);
		const uint32_t desired_meshlet_capacity = std::max(required_meshlet_count, 1024u);

		if (!frame_resource.instance_data.is_valid() || frame_resource.instance_capacity < desired_instance_capacity) {
			if (frame_resource.instance_data.is_valid())
				rhi->destroy_buffer(frame_resource.instance_data);
			// Host-visible + mapped (UnorderedAccess) so the per-frame instance
			// data can be written directly from the CPU, bypassing the async
			// staging/upload path that caused texture flicker.
			frame_resource.instance_data = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_instance_capacity) * instance_data_stride, ResourceState::UnorderedAccess);
			frame_resource.instance_capacity = desired_instance_capacity;
		}

		if (enable_gpu_driven) {
			if (!frame_resource.indirect_instance.is_valid() || frame_resource.indirect_capacity < desired_indirect_capacity) {
				if (frame_resource.indirect_instance.is_valid())
					rhi->destroy_buffer(frame_resource.indirect_instance);
				frame_resource.indirect_instance = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_indirect_capacity) * indirect_instance_stride, ResourceState::UnorderedAccess);
			}

			if (!frame_resource.indirect_draw.is_valid() || frame_resource.indirect_capacity < desired_indirect_capacity) {
				if (frame_resource.indirect_draw.is_valid())
					rhi->destroy_buffer(frame_resource.indirect_draw);
				frame_resource.indirect_draw = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_indirect_capacity) * indirect_draw_stride, ResourceState::IndirectArgument);
			}

			if (!frame_resource.csm_indirect_draw.is_valid() || frame_resource.csm_indirect_capacity < desired_scene_capacity) {
				if (frame_resource.csm_indirect_draw.is_valid())
					rhi->destroy_buffer(frame_resource.csm_indirect_draw);
				// MAX_CASCADES is 4, we allocate 4x capacity.
				frame_resource.csm_indirect_draw = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_scene_capacity) * 4 * indirect_draw_stride, ResourceState::IndirectArgument);
				frame_resource.csm_indirect_capacity = desired_scene_capacity;
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
				frame_resource.csm_instance_data = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_scene_capacity) * indirect_instance_stride, ResourceState::UnorderedAccess);
				frame_resource.csm_instance_capacity = desired_scene_capacity;
			}

			// Full-scene InstanceData (model+material) matching the reordered
			// full-scene DrawData, used by shadow.vert during CSM GPU draws.
			// Created as UnorderedAccess so it is host-visible+mapped (CPU can
			// write the reordered models directly, bypassing async staging).
			if (!frame_resource.csm_instance_models.is_valid() || frame_resource.csm_instance_models_capacity < desired_scene_capacity) {
				if (frame_resource.csm_instance_models.is_valid())
					rhi->destroy_buffer(frame_resource.csm_instance_models);
				frame_resource.csm_instance_models = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_scene_capacity) * instance_data_stride, ResourceState::UnorderedAccess);
				frame_resource.csm_instance_models_capacity = desired_scene_capacity;
			}

			if (!frame_resource.stats_readback.is_valid()) {
				frame_resource.stats_readback = rhi->create_gpu_buffer(stats_buffer_size, ResourceState::UnorderedAccess);
			}

			if (!frame_resource.meshlet_frustum_stats.is_valid()) {
				frame_resource.meshlet_frustum_stats = rhi->create_gpu_buffer(stats_buffer_size, ResourceState::UnorderedAccess);
			}

			if (!frame_resource.meshlet_hiz_stats.is_valid()) {
				frame_resource.meshlet_hiz_stats = rhi->create_gpu_buffer(stats_buffer_size, ResourceState::UnorderedAccess);
			}

			frame_resource.indirect_capacity = desired_indirect_capacity;
		}

		if (enable_meshlets) {
			if (!frame_resource.meshlet_visibility.is_valid() || frame_resource.meshlet_visibility_capacity < desired_meshlet_capacity) {
				if (frame_resource.meshlet_visibility.is_valid())
					rhi->destroy_buffer(frame_resource.meshlet_visibility);
				frame_resource.meshlet_visibility = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_meshlet_capacity) * sizeof(uint32_t), ResourceState::UnorderedAccess);
			}

			if (!frame_resource.meshlet_hiz_visibility.is_valid() || frame_resource.meshlet_visibility_capacity < desired_meshlet_capacity) {
				if (frame_resource.meshlet_hiz_visibility.is_valid())
					rhi->destroy_buffer(frame_resource.meshlet_hiz_visibility);
				frame_resource.meshlet_hiz_visibility = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_meshlet_capacity) * sizeof(uint32_t), ResourceState::UnorderedAccess);
			}

			frame_resource.meshlet_visibility_capacity = desired_meshlet_capacity;
		}
	}

	GPUScene::PagePool& GPUScene::get_page_pool() { return page_pool; }
	const GPUScene::PagePool& GPUScene::get_page_pool() const { return page_pool; }
	BufferHandle GPUScene::get_page_pool_buffer() const { return page_pool.page_pool_buffer; }
	BufferHandle GPUScene::get_page_table_buffer() const { return page_table_buffer; }

	void GPUScene::update_page_table_entry(uint32_t page_index, uint32_t pool_offset, uint32_t valid) {
		if (!page_table_buffer.is_valid() || page_index >= max_page_table_entries)
			return;
		if (!page_table_buffer.mapped_ptr) return;
		auto* entries = static_cast<PageTableEntry*>(page_table_buffer.mapped_ptr);
		entries[page_index].valid = valid;
		entries[page_index].pool_offset = pool_offset;
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
