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
namespace bud::physics { class ClothSystem; }

namespace bud::graphics {
	struct MeshAssetHandle {
		static constexpr uint32_t invalid_id = std::numeric_limits<uint32_t>::max();

		uint32_t mesh_id;
		uint32_t material_id;
		// Reserved at enqueue time inside upload_mesh: destination vertex offset of
		// this mesh's vertices in the mega vertex buffer (valid even though the GPU
		// upload itself is queued; -1 only for the invalid handle).
		int32_t vertex_offset = -1;

        static MeshAssetHandle invalid() {
            return { invalid_id, invalid_id, -1 };
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
		bool is_taa_ready() const;

		const void* get_readback_pixels() const;

		// Game-thread safe snapshot (CPU-side bounds only)
		std::vector<bud::math::AABB> get_mesh_bounds_snapshot() const;
		std::vector<std::vector<bud::math::AABB>> get_submesh_bounds_snapshot() const;
		void register_mesh_bounds(uint32_t mesh_id, const bud::math::AABB& aabb);
		// Update bounds WITHOUT touching is_page_based (used by simulated cloth meshes
		// which must stay on the traditional Range-B draw path).
		void update_mesh_bounds(uint32_t mesh_id, const bud::math::AABB& aabb);

		void update_physics_debug_vertices(const std::vector<PhysicsDebugVertex>& verts);

		GPUScene& get_gpu_scene() { return gpu_scene; }
		RHI* get_rhi() { return rhi; }
		bud::physics::ClothSystem* get_cloth_system() { return cloth_system.get(); }
		uint32_t register_page_based_mesh(uint32_t page_index, uint32_t cluster_count,
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

		// Shadow reach hysteresis (see update_cascades): scene_bounds change while pages
		// stream, and a per-frame changing shadow_far would rescale every cascade's texel
		// footprint and make the shadows creep. We only re-derive when it moved >10%.
		float cached_shadow_far = -1.0f;
		float cached_shadow_far_plane = -1.0f;
		float cached_shadow_far_scene_factor = -1.0f;

		// Per-cascade reach (ortho half extent) anchors with a dead zone, so the shadow
		// texel grid stays bit-constant while the camera only moves/rotates.
		// Index i is only valid while cascade i is configured.
		float cascade_reach_anchor_[MAX_CASCADES] = { -1.0f, -1.0f, -1.0f, -1.0f };
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
		std::unique_ptr<ForwardTranslucentPass> forward_translucent_pass;
		std::unique_ptr<UIPass> ui_pass;
		std::unique_ptr<VisibilityPass> visibility_pass;
		std::unique_ptr<ScreenSpaceReflectionPass> ssr_pass;
		std::unique_ptr<ScreenSpaceGlobalIlluminationPass> ssgi_pass;
		std::unique_ptr<ResolvePass> resolve_pass;
		std::unique_ptr<TAAPass> taa_pass;
		std::unique_ptr<PhysicsDebugPass> physics_debug_pass;
		std::unique_ptr<ClothDebugPass> cloth_debug_pass;
		std::unique_ptr<bud::physics::ClothSystem> cloth_system;

		bool has_mesh_shader = false;

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
		std::unordered_map<std::string, uint32_t> bound_texture_slots;
		mutable std::mutex texture_slot_mutex;
		bool graphviz_exported_ = false;

	public:
		struct HierarchyInstance {
			bud::math::mat4 model_matrix;
			uint32_t mesh_id;
			uint32_t material_id;
			uint32_t root_group_index;
			uint32_t flags;
			bud::math::vec3 global_sphere_center;
			float global_sphere_radius;
			float error_threshold;
			uint32_t base_virtual_page;
			uint32_t padding[2];
		};
		
		struct InstanceData {
			bud::math::mat4 model;
			uint32_t material_id;
			uint32_t page_slot;
			float blend_factor; // 0.0 = full high LOD, 1.0 = full low LOD
			uint32_t padding;
		};

		std::shared_ptr<UploadQueue> upload_queue;

        // Headless Offscreen Rendering
        bud::graphics::TextureHandle offscreen_target{};
        std::vector<bud::graphics::BufferHandle> readback_buffers;
	};
}
