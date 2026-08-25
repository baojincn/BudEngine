#pragma once

#include <memory>
#include <atomic>
#include <vector>
#include <mutex>
#include <limits>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

namespace bud::streaming { class StreamingManager; }

namespace bud::graphics {
	struct MeshAssetHandle {
		static constexpr uint32_t invalid_id = std::numeric_limits<uint32_t>::max();

		uint32_t mesh_id;
		uint32_t material_id;

        static MeshAssetHandle invalid() {
            return { invalid_id, invalid_id };
        }
        bool is_valid() const {
            if (mesh_id != invalid_id)
                return true;
            return false;
        }
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

		const void* get_readback_pixels() const;

		// Game-thread safe snapshot (CPU-side bounds only)
		std::vector<bud::math::AABB> get_mesh_bounds_snapshot() const;
		std::vector<std::vector<bud::math::AABB>> get_submesh_bounds_snapshot() const;

		GPUScene& get_gpu_scene() { return gpu_scene; }
		RHI* get_rhi() { return rhi; }
		uint32_t register_page_based_mesh(uint32_t page_index, uint32_t meshlet_count,
			uint32_t index_count, const bud::math::AABB& aabb, const bud::math::AABB& global_aabb,
			uint32_t vertex_data_offset, uint32_t index_data_offset,
			const std::vector<PageSubMesh>& page_submeshes,
			const std::vector<std::pair<uint32_t, uint32_t>>& lod_index_ranges,
			const float lod_errors[3] = nullptr);

		uint32_t bind_texture_async(const std::string& path);

		// 线程安全：将 RHI 命令推入 UploadQueue，将在下一帧 render pass 前的 flush_upload_queue() 中执行。
		// 用于 StreamingManager 等异步系统避免在 worker 线程直接调用 single-time commands。
		void enqueue_rhi_command(std::function<void()> cmd);

		void set_streaming_manager(bud::streaming::StreamingManager* sm) { streaming_manager = sm; }

		private:
		struct UploadQueue {
			std::mutex mutex;
			std::vector<std::function<void()>> commands;
		};

		void update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb);
		void select_occluders_cpu(const RenderScene& render_scene, const SceneView& view, const std::vector<SortItem>& source_list, size_t source_count, std::vector<SortItem>& out_occluders, size_t out_count);

		RHI* rhi;
		RenderGraph render_graph;
		RenderConfig render_config;
		bud::io::AssetManager* asset_manager;
		bud::threading::TaskScheduler* task_scheduler;
		bud::streaming::StreamingManager* streaming_manager = nullptr;

        std::unique_ptr<CSMShadowPass> csm_pass;
        std::unique_ptr<DepthOnlyPass> depth_only_pass;
		std::unique_ptr<AmbientOcclusionPass> ao_pass;
		std::unique_ptr<AOTemporalPass> ao_temporal_pass;
		std::unique_ptr<AOBlurPass> ao_blur_pass;
		std::unique_ptr<PyramidMipPass> pyramid_mip_pass;
		std::unique_ptr<PyramidMipDebugPass> pyramid_mip_debug_pass;
		std::unique_ptr<InstanceCullingPass> instance_culling_pass;
		std::unique_ptr<HierarchyTraversalPass> hierarchy_traversal_pass;
		std::unique_ptr<PageEmitPass> page_emit_pass;
		std::unique_ptr<ClusterCullPass> cluster_cull_pass;
		std::unique_ptr<ClusterVisualizationPass> cluster_visualization_pass;
		std::unique_ptr<MainPass> main_pass;
		std::unique_ptr<UIPass> ui_pass;

		PipelineHandle csm_cull_pipeline;
		GPUStats last_gpu_stats{};
		GPUScene gpu_scene;

		std::vector<RenderMesh> meshes;
		std::vector<bud::math::AABB> mesh_bounds;
		mutable std::mutex mesh_bounds_mutex;
		mutable std::mutex mesh_mutex;

		std::vector<SortItem> sort_list;

		// Persistent storage for temporary per-frame occluder lists to ensure lifetime
		// when render passes capture references to the list.
		std::vector<SortItem> persistent_occluder_list;

		std::atomic<uint32_t> next_bindless_slot{ 1 };
		std::atomic<uint32_t> next_mesh_id{ 0 };

		struct HierarchyInstance {
			uint32_t mesh_id;
			uint32_t material_id;
			uint32_t root_group_index;
			uint32_t flags;
			bud::math::vec3 global_sphere_center;
			float global_sphere_radius;
			float error_threshold;
			uint32_t base_virtual_page;
			uint32_t padding[2];
			bud::math::mat4 model_matrix;
		};
		
		struct InstanceData {
			bud::math::mat4 model;
			uint32_t material_id;
			uint32_t page_slot;
			uint32_t padding[2];
		};

		std::shared_ptr<UploadQueue> upload_queue;

        // Headless Offscreen Rendering
        bud::graphics::TextureHandle offscreen_target{};
        std::vector<bud::graphics::BufferHandle> readback_buffers;
	};
}
