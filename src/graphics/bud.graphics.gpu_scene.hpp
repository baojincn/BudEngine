#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "src/graphics/bud.graphics.memory.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"

namespace bud::graphics {

	class GPUScene {
	public:
		struct GeometryPool {
			static constexpr uint64_t kVertexPoolSize = 256ull * 1024 * 1024;
			static constexpr uint64_t kIndexPoolSize = 128ull * 1024 * 1024;

			BufferHandle vertex_buffer;
			BufferHandle index_buffer;
			std::atomic<uint32_t> next_vertex{ 0 };
			std::atomic<uint32_t> next_index{ 0 };
			bool initialized = false;
		};

		struct FrameResources {
			BufferHandle instance_data;
			BufferHandle indirect_instance;
			BufferHandle indirect_draw;
			BufferHandle stats_readback;
			BufferHandle meshlet_frustum_stats;
			BufferHandle meshlet_hiz_stats;
			BufferHandle meshlet_visibility;
			BufferHandle meshlet_hiz_visibility;
			uint32_t instance_capacity = 0;
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

		FrameResources& frame_resources(uint32_t frame_index);
		const FrameResources& frame_resources(uint32_t frame_index) const;

		void ensure_frame_resources(RHI* rhi,
			uint32_t frame_index,
			uint32_t required_instance_count,
			uint32_t required_draw_count,
			uint32_t required_meshlet_count,
			uint64_t instance_data_stride,
			uint64_t indirect_instance_stride,
			uint64_t indirect_draw_stride,
			uint64_t stats_buffer_size,
			bool enable_gpu_driven,
			bool enable_meshlets);

	private:
		GeometryPool geometry_pool_;
		std::vector<FrameResources> frame_resources_;
	};
}
