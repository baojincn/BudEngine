#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <limits>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"
namespace bud::graphics {
	struct MeshAssetHandle {
		static constexpr uint32_t invalid_id = std::numeric_limits<uint32_t>::max();

		uint32_t mesh_id;
		uint32_t material_id;

		static MeshAssetHandle invalid() { return { invalid_id, invalid_id }; }
		bool is_valid() const { return mesh_id != invalid_id; }
	};

	class Renderer {
	public:
		Renderer(RHI* rhi, bud::io::AssetManager* asset_manager, bud::threading::TaskScheduler* task_scheduler);
		~Renderer();

		MeshAssetHandle upload_mesh(const bud::io::MeshData& mesh_data);

		// Only work on Rendering Thread
		void flush_upload_queue();
		void update_ui_draw_data(ImDrawData* draw_data);

		void render(const bud::graphics::RenderScene& render_scene, SceneView& scene_view);

		void set_config(const RenderConfig& config);
		const RenderConfig& get_config() const;

		// Game-thread safe snapshot (CPU-side bounds only)
		std::vector<bud::math::AABB> get_mesh_bounds_snapshot() const;
		std::vector<std::vector<bud::math::AABB>> get_submesh_bounds_snapshot() const;

	private:
		struct UploadQueue {
			std::mutex mutex;
			std::vector<std::function<void()>> commands;
		};

		// Global Geometry Pool (Mega-Buffer) that all meshes are packed into
		struct GeometryPool {
			static constexpr uint64_t kVertexPoolSize = 256ull * 1024 * 1024; // 256 MB
			static constexpr uint64_t kIndexPoolSize  = 128ull * 1024 * 1024; // 128 MB

			bud::graphics::BufferHandle vertex_buffer;
			bud::graphics::BufferHandle index_buffer;

			std::atomic<uint32_t> next_vertex{ 0 }; // in vertices
			std::atomic<uint32_t> next_index{ 0 };  // in indices

			bool initialized = false;
		};

		void update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb);

		RHI* rhi;
		RenderGraph render_graph;
		RenderConfig render_config;
		bud::io::AssetManager* asset_manager;
		bud::threading::TaskScheduler* task_scheduler;

		std::unique_ptr<CSMShadowPass> csm_pass;
		std::unique_ptr<ZPrepass> z_prepass;
		std::unique_ptr<HiZMipPass> hiz_mip_pass;
		std::unique_ptr<HiZCullingPass> hiz_pass;
		std::unique_ptr<HiZDebugPass> hiz_debug_pass;
		std::unique_ptr<MainPass> main_pass;
		std::unique_ptr<ClusterVisualizationPass> cluster_viz_pass;
		std::unique_ptr<UIPass> ui_pass;

		// GPU-Driven specific (Per-frame)
		uint32_t current_indirect_capacity = 0;
		std::vector<bud::graphics::BufferHandle> indirect_instance_buffers;
		std::vector<bud::graphics::BufferHandle> indirect_draw_buffers;
		std::vector<bud::graphics::BufferHandle> stats_readback_buffers;

		GPUStats last_gpu_stats{};

		GeometryPool geometry_pool;

		std::vector<RenderMesh> meshes;
		std::vector<bud::math::AABB> mesh_bounds;
		mutable std::mutex mesh_bounds_mutex;

		std::vector<SortItem> sort_list;

		std::atomic<uint32_t> next_bindless_slot{ 1 };
		std::atomic<uint32_t> next_mesh_id{ 0 };

		struct InstanceData {
			bud::math::mat4 model;
			uint32_t material_id;
			uint32_t padding[3];
		};

		std::vector<bud::graphics::BufferHandle> instance_data_ssbos;
		uint32_t instance_data_capacity = 0;

		std::shared_ptr<UploadQueue> upload_queue;
	};
}
