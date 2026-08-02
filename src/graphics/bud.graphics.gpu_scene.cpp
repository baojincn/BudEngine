#include "src/graphics/bud.graphics.gpu_scene.hpp"

#include <algorithm>

namespace bud::graphics {

	void GPUScene::init(RHI* rhi, uint32_t inflight_frame_count) {
		frame_resources_.resize(inflight_frame_count);

		if (!rhi || geometry_pool_.initialized) {
			return;
		}

		geometry_pool_.vertex_buffer = rhi->create_gpu_buffer(GeometryPool::kVertexPoolSize, ResourceState::VertexBuffer);
		geometry_pool_.index_buffer = rhi->create_gpu_buffer(GeometryPool::kIndexPoolSize, ResourceState::IndexBuffer);
		geometry_pool_.initialized = geometry_pool_.vertex_buffer.is_valid() && geometry_pool_.index_buffer.is_valid();

		if (!page_pool_.initialized) {
			page_pool_.page_pool_buffer = rhi->create_gpu_buffer(PagePool::kPagePoolSize, ResourceState::UnorderedAccess);
			page_pool_.initialized = page_pool_.page_pool_buffer.is_valid();
			if (page_pool_.initialized) {
				std::lock_guard lock(page_pool_.mutex);
				page_pool_.free_slots.reserve(PagePool::kMaxPages);
				for (int32_t i = PagePool::kMaxPages - 1; i >= 0; --i)
					page_pool_.free_slots.push_back(i);
			}
		}

		if (!page_table_buffer_.is_valid()) {
			page_table_buffer_ = rhi->create_gpu_buffer(kMaxPageTableEntries * sizeof(PageTableEntry), ResourceState::UnorderedAccess);
		}
	}

	void GPUScene::shutdown(RHI* rhi) {
		if (rhi && geometry_pool_.initialized) {
			if (geometry_pool_.vertex_buffer.is_valid())
				rhi->destroy_buffer(geometry_pool_.vertex_buffer);
			if (geometry_pool_.index_buffer.is_valid())
				rhi->destroy_buffer(geometry_pool_.index_buffer);
		}

		geometry_pool_.vertex_buffer = {};
		geometry_pool_.index_buffer = {};
		geometry_pool_.next_vertex.store(0, std::memory_order_relaxed);
		geometry_pool_.next_index.store(0, std::memory_order_relaxed);
		geometry_pool_.initialized = false;

		if (rhi && page_pool_.initialized) {
			if (page_pool_.page_pool_buffer.is_valid())
				rhi->destroy_buffer(page_pool_.page_pool_buffer);
		}
		page_pool_.page_pool_buffer = {};
		{
			std::lock_guard lock(page_pool_.mutex);
			page_pool_.free_slots.clear();
		}
		page_pool_.initialized = false;

		if (rhi && page_table_buffer_.is_valid()) {
			rhi->destroy_buffer(page_table_buffer_);
		}
		page_table_buffer_ = {};

		if (rhi) {
			for (auto& frame_resource : frame_resources_) {
				if (frame_resource.instance_data.is_valid()) rhi->destroy_buffer(frame_resource.instance_data);
				if (frame_resource.indirect_instance.is_valid()) rhi->destroy_buffer(frame_resource.indirect_instance);
				if (frame_resource.indirect_draw.is_valid()) rhi->destroy_buffer(frame_resource.indirect_draw);
				if (frame_resource.stats_readback.is_valid()) rhi->destroy_buffer(frame_resource.stats_readback);
				if (frame_resource.meshlet_frustum_stats.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_frustum_stats);
				if (frame_resource.meshlet_hiz_stats.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_hiz_stats);
				if (frame_resource.meshlet_visibility.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_visibility);
				if (frame_resource.meshlet_hiz_visibility.is_valid()) rhi->destroy_buffer(frame_resource.meshlet_hiz_visibility);
				frame_resource = {};
			}
		}

		mesh_geometry_.clear();
		frame_resources_.clear();
	}

	GPUScene::GeometryPool& GPUScene::geometry_pool() {
		return geometry_pool_;
	}

	const GPUScene::GeometryPool& GPUScene::geometry_pool() const {
		return geometry_pool_;
	}

	BufferHandle GPUScene::get_vertex_buffer() const {
		return geometry_pool_.vertex_buffer;
	}

	BufferHandle GPUScene::get_index_buffer() const {
		return geometry_pool_.index_buffer;
	}

	void GPUScene::set_mesh_geometry(uint32_t mesh_id, uint32_t first_index, int32_t vertex_offset) {
		if (mesh_geometry_.size() <= mesh_id) {
			mesh_geometry_.resize(mesh_id + 1);
		}

		mesh_geometry_[mesh_id].first_index = first_index;
		mesh_geometry_[mesh_id].vertex_offset = vertex_offset;
	}

	const GPUScene::MeshGeometry& GPUScene::mesh_geometry(uint32_t mesh_id) const {
		return mesh_geometry_.at(mesh_id);
	}

	GPUScene::FrameResources& GPUScene::frame_resources(uint32_t frame_index) {
		return frame_resources_.at(frame_index);
	}

	const GPUScene::FrameResources& GPUScene::frame_resources(uint32_t frame_index) const {
		return frame_resources_.at(frame_index);
	}

	void GPUScene::ensure_frame_resources(RHI* rhi,
		uint32_t frame_index,
		uint32_t required_instance_count,
		uint32_t required_draw_count,
		uint32_t required_meshlet_count,
		uint64_t instance_data_stride,
		uint64_t indirect_instance_stride,
		uint64_t indirect_draw_stride,
		uint64_t stats_buffer_size,
		bool enable_gpu_driven,
		bool enable_meshlets)
	{
		if (!rhi || frame_index >= frame_resources_.size()) {
			return;
		}

		auto& frame_resource = frame_resources_[frame_index];
		const uint32_t desired_instance_capacity = std::max(required_instance_count + 1024u, 1024u);
		const uint32_t desired_indirect_capacity = std::max(required_draw_count + 1024u, 1024u);
		const uint32_t desired_meshlet_capacity = std::max(required_meshlet_count, 1024u);

		if (!frame_resource.instance_data.is_valid() || frame_resource.instance_capacity < desired_instance_capacity) {
			if (frame_resource.instance_data.is_valid())
				rhi->destroy_buffer(frame_resource.instance_data);
			frame_resource.instance_data = rhi->create_gpu_buffer(static_cast<uint64_t>(desired_instance_capacity) * instance_data_stride, ResourceState::ShaderResource);
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

	GPUScene::PagePool& GPUScene::get_page_pool() { return page_pool_; }
	const GPUScene::PagePool& GPUScene::get_page_pool() const { return page_pool_; }
	BufferHandle GPUScene::get_page_pool_buffer() const { return page_pool_.page_pool_buffer; }
	BufferHandle GPUScene::get_page_table_buffer() const { return page_table_buffer_; }

	void GPUScene::update_page_table_entry(uint32_t page_index, uint32_t pool_offset) {
		if (!page_table_buffer_.is_valid() || page_index >= kMaxPageTableEntries)
			return;
		if (!page_table_buffer_.mapped_ptr) return;
		auto* entries = static_cast<PageTableEntry*>(page_table_buffer_.mapped_ptr);
		entries[page_index].valid = 1;
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
