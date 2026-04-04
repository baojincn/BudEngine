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
		void request_meshlet_rendering_enabled(bool enabled);
		bool is_meshlet_rendering_enabled() const;

		const void* get_readback_pixels() const;

		// Game-thread safe snapshot (CPU-side bounds only)
		std::vector<bud::math::AABB> get_mesh_bounds_snapshot() const;
		std::vector<std::vector<bud::math::AABB>> get_submesh_bounds_snapshot() const;

	private:
		struct UploadQueue {
			std::mutex mutex;
			std::vector<std::function<void()>> commands;
		};

		void update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb);

		// CPU heuristic occluder selection (prototype)
		void select_occluders_cpu(const RenderScene& render_scene, const SceneView& view, const std::vector<SortItem>& source_list, size_t source_count, std::vector<SortItem>& out_occluders, size_t out_count);

		RHI* rhi;
		RenderGraph render_graph;
		RenderConfig render_config;
		bud::io::AssetManager* asset_manager;
		bud::threading::TaskScheduler* task_scheduler;

        std::unique_ptr<CSMShadowPass> csm_pass;
        std::unique_ptr<DepthOnlyPass> depth_only_pass;
		std::unique_ptr<HiZMipPass> hiz_mip_pass;
		std::unique_ptr<HiZCullingPass> hiz_pass;
		std::unique_ptr<MeshletFrustumCullingPass> meshlet_frustum_pass;
		std::unique_ptr<HeuristicOccluderSelectionPass> heuristic_occluder_pass;
		std::unique_ptr<MeshletHiZCullingPass> meshlet_hiz_pass;
		std::unique_ptr<MeshletIndirectEmissionPass> meshlet_indirect_pass;
		std::unique_ptr<HiZDebugPass> hiz_debug_pass;
		std::unique_ptr<MainPass> main_pass;
		std::unique_ptr<ClusterVisualizationPass> cluster_viz_pass;
		std::unique_ptr<UIPass> ui_pass;

		std::atomic<bool> meshlet_rendering_enabled{ true };
		std::atomic<bool> meshlet_rendering_toggle_pending{ false };
		std::atomic<bool> meshlet_rendering_toggle_value{ true };

		GPUStats last_gpu_stats{};
		GPUScene gpu_scene;

		std::vector<RenderMesh> meshes;
		std::vector<bud::math::AABB> mesh_bounds;
		mutable std::mutex mesh_bounds_mutex;

		std::vector<SortItem> sort_list;

		// Persistent storage for temporary per-frame occluder lists to ensure lifetime
		// when render passes capture references to the list.
		std::vector<SortItem> persistent_occluder_list;

		std::atomic<uint32_t> next_bindless_slot{ 1 };
		std::atomic<uint32_t> next_mesh_id{ 0 };

		struct InstanceData {
			bud::math::mat4 model;
			uint32_t material_id;
			uint32_t padding[3];
		};

		std::shared_ptr<UploadQueue> upload_queue;

        // Headless Offscreen Rendering
        bud::graphics::Texture* offscreen_target = nullptr;
        std::vector<bud::graphics::BufferHandle> readback_buffers;
	};
}
