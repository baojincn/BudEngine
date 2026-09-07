#pragma once

#include <vector>
#include <mutex>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "src/core/bud.math.hpp"
#include "src/io/bud.io.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"

namespace bud::graphics {
	class GPUScene;
	class RenderPassBase {
	public:
		virtual ~RenderPassBase() = default;
		virtual void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) = 0;
		virtual void shutdown(RHI* rhi) {}
	};

	class RenderPass : public RenderPassBase {
	protected:
		PipelineHandle pipeline;

		// Helper for asynchronous multi-shader loading
		void load_shaders_async(bud::io::AssetManager* asset_manager, 
							   const std::vector<std::string>& paths, 
							   std::function<void(std::vector<std::vector<char>>)> on_loaded);

	public:
        virtual ~RenderPass() = default;
        bool is_ready() const {
            return pipeline.is_valid();
        }
		void shutdown(RHI* rhi) override;
	};

	class InstanceCullingPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle instance_buffer, RGHandle indirect_draw_buffer, RGHandle stats_buffer, RGHandle hiz_pyramid, const SceneView& view, size_t instance_count);
	};

	class PyramidMipPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const RenderConfig& config, RGHandle target_pyramid = {});
	};

	class PyramidMipDebugPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle hiz_pyramid, uint32_t mip_level);
	};

    class DepthOnlyPass : public RenderPass {
    public:
        void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle backbuffer,
			const RenderScene& render_scene,
			const SceneView& view,
			const RenderConfig& config,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list,
			size_t instance_count,
			RGHandle indirect_draw_buffer,
			const GPUScene& gpu_scene,
			bud::graphics::BufferHandle mega_vertex_buffer,
			bud::graphics::BufferHandle mega_index_buffer,
			RGHandle prev_depth_buffer = {},
			size_t split_index = 0);
    };


	class HierarchyTraversalPass : public RenderPass {
		PipelineHandle hierarchy_traversal_pipeline;

	public:
		~HierarchyTraversalPass();
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(
			RenderGraph& rg,
			const SceneView& view,
			const RenderConfig& config,
			const RenderScene& render_scene,
			const std::vector<RenderMesh>& meshes,
			size_t instance_count,
			const GPUScene& gpu_scene,
			uint32_t current_frame,
			uint32_t cascade_index = 0,
			float lod_error_scale = 1.0f,
			float ortho_extent = 0.0f,
			BufferHandle target_visible_pages = {},
			const std::string& pass_name = "Hierarchy Traversal",
			// Optional instance source. CSM cascade traversals pass a FULL-SCENE
			// HierarchyInstance buffer here; the default (invalid) keeps the main-view
			// visible-instance buffer (frame.instance_data).
			BufferHandle source_instances = {}
		);
	};

	class PageEmitPass : public RenderPass {
		PipelineHandle page_emit_pipeline;

	public:
		~PageEmitPass();
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void add_to_graph(RenderGraph& rg, const RenderConfig& config, const GPUScene& gpu_scene, uint32_t current_frame);
	};

	class ClusterCullPass : public RenderPass {
		PipelineHandle cluster_cull_pipeline;
		PipelineHandle clear_stats_pipeline;

	public:
		~ClusterCullPass();
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle hiz_pyramid, RGHandle rg_draw, const SceneView& view, const RenderConfig& config, const GPUScene& gpu_scene, uint32_t current_frame);
	};

	class CSMShadowPass : public RenderPass {
		PipelineHandle csm_cull_pipeline;
		PipelineHandle shadow_mesh_pipeline;
		uint64_t shadow_visibility_set_layout = 0;
		RHI* stored_rhi = nullptr;

	public:
		~CSMShadowPass();
		void shutdown(RHI* rhi) override;

		struct ShadowData {
			bud::math::mat4 light_space_matrix;
			bud::math::vec4 light_dir;
		};

		// Shadow caster work lists. The renderer owns the layout knowledge, the pass
		// just obeys it - the previous code guessed the indirect-block stride from an
		// unrelated count and picked the wrong item range, which corrupted every
		// cascade >= 1 (and skipped traditional casters entirely when VG items existed).
		struct ShadowCasterRange {
			uint32_t stride_commands = 0;	// csm_cull's total_instances == entries per cascade block
			uint32_t first_command = 0;		// start of the range to issue inside each block
			uint32_t command_count = 0;		// commands to issue per cascade
			bool Valid() const { return stride_commands > 0 && command_count > 0; }
		};

		struct ShadowCasterLists {
			// GPU-driven traditional (non page-based) casters, fed to csm_cull.comp.
			// VG mode   : the full-scene list [0, scene_split)   (stride = scene_split)
			// non-VG mode: the opaque sorted range [range_a, range_a+range_b) (stride = visible)
			ShadowCasterRange traditional;
			// Translucent meshes are not rasterized into the shadow map: they render in
			// the forward translucent pass, and casting from them would need a shadow FS
			// with opacity-aware depth (not implemented). Entries carry DrawData bit 2 and
			// the cull shader drops them, so this list also excludes them explicitly.
			bool skip_translucent_casters = true;
		};

		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(
			RenderGraph& rg,
			const SceneView& view,
			const RenderConfig& config,
			const RenderScene& render_scene,
			const std::vector<RenderMesh>& meshes,
			std::vector<std::vector<uint32_t>> csm_visible_instances,
			const GPUScene& gpu_scene,
			bud::graphics::BufferHandle mega_vertex_buffer,
			bud::graphics::BufferHandle mega_index_buffer,
			bud::graphics::RGHandle rg_instance_data,
			const ShadowCasterLists& casters = {},
			bud::graphics::RGHandle rg_indirect_draw = {},
			std::array<bud::graphics::RGHandle, MAX_CASCADES> rg_csm_visible_pages = {},
			// VG instance list that matches rg_csm_visible_pages' instance_id space
			// (full-scene when shadow_full_scene_casters is on, otherwise empty =
			// fall back to the main-view visible instances).
			bud::graphics::BufferHandle vg_shadow_instances = {}
		);
	};

	class ForwardTranslucentPass : public RenderPass {
	public:
		PipelineHandle pipeline_wireframe;
		bool is_ready() const { return pipeline.is_valid() && pipeline_wireframe.is_valid(); }
		void shutdown(RHI* rhi) override;

		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer, RGHandle depth_buffer,
			const RenderScene& render_scene,
			const SceneView& view,
			const RenderConfig& config,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list,
			const SceneDrawRanges& ranges,
			bud::graphics::RGHandle indirect_draw_buffer,
			bud::graphics::RGHandle instance_data,
			const GPUScene& gpu_scene,
			bud::graphics::BufferHandle mega_vertex_buffer,
			bud::graphics::BufferHandle mega_index_buffer,
			bud::graphics::RGHandle ao_map = {},
			bud::graphics::RGHandle ssr_map = {},
			bud::graphics::RGHandle ssgi_map = {},
			bud::graphics::RGHandle opaque_scene_color = {});
	};

	struct UIDrawCmdSnapshot {
		ImVec4 clip_rect{};
		uint32_t elem_count = 0;
		uint32_t idx_offset = 0;
		uint32_t vtx_offset = 0;
		uint32_t texture_id = 0;
	};

	struct UIDrawListSnapshot {
		std::vector<ImDrawVert> vertices;
		std::vector<uint32_t> indices;
		std::vector<UIDrawCmdSnapshot> commands;
	};

	struct UIDrawDataSnapshot {
		ImVec2 display_pos{};
		ImVec2 display_size{};
		ImVec2 framebuffer_scale{ 1.0f, 1.0f };
		std::vector<UIDrawListSnapshot> lists;

        bool has_data() const {
            return !lists.empty();
        }

		uint32_t total_vtx_count() const {
			uint32_t total = 0;
			for (const auto& list : lists)
				total += static_cast<uint32_t>(list.vertices.size());
			return total;
		}

		uint32_t total_idx_count() const {
			uint32_t total = 0;
			for (const auto& list : lists)
				total += static_cast<uint32_t>(list.indices.size());
			return total;
		}
	};

	class UIPass : public RenderPass {
		TextureHandle font_texture;

		uint32_t font_bindless_index = 0;
		std::mutex draw_data_mutex;
		UIDrawDataSnapshot cached_draw_data;

		std::vector<BufferHandle> vertex_buffers;
		std::vector<BufferHandle> index_buffers;
		std::vector<uint32_t> current_vertex_buffer_sizes;
		std::vector<uint32_t> current_index_buffer_sizes;

	public:
		~UIPass();
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void update_draw_data(ImDrawData* draw_data);
		void add_to_graph(RenderGraph& rg, RGHandle backbuffer);
	};

	class AmbientOcclusionPass : public RenderPass {
		PipelineHandle ssao_pipeline;
		PipelineHandle gtao_pipeline;

	public:
		~AmbientOcclusionPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config);
	};

	class AOBlurPass : public RenderPass {
	public:
		~AOBlurPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle raw_ao, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config);
	};

	class AOTemporalPass : public RenderPass {
		// Ping-pong history textures
		TextureHandle history_textures[2];
		uint32_t history_read_index = 0;
		bool has_valid_history = false;

		// Previous frame camera matrix for reprojection.
		bud::math::mat4 last_view_proj = bud::math::mat4(1.0f);
		bool has_last_view_proj = false;

		RHI* stored_rhi = nullptr;
		uint32_t history_width = 0;
		uint32_t history_height = 0;

	public:
		~AOTemporalPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle raw_ao, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config);
	};

	class VisibilityPass : public RenderPass {
		PipelineHandle visibility_pipeline;
		PipelineHandle visibility_pipeline_wireframe;
		PipelineHandle visibility_indirect_pipeline;
		PipelineHandle visibility_indirect_pipeline_wireframe;
		uint64_t visibility_set_layout = 0;
		uint64_t visibility_descriptor_set = 0;

	public:
		~VisibilityPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle depth_buffer,
			const SceneView& view,
			const RenderConfig& config,
			RGHandle rg_visible_pages,
			RGHandle rg_hiz_pyramid,
			const GPUScene& gpu_scene,
			const SceneDrawRanges& ranges = {},
			BufferHandle mega_vertex_buffer = {},
			BufferHandle mega_index_buffer = {},
			RGHandle rg_draw = {},
			RGHandle* out_depth = nullptr);
		void add_phase2_to_graph(RenderGraph& rg,
			RGHandle visibility_buffer,
			RGHandle depth_buffer,
			const SceneView& view,
			const RenderConfig& config,
			RGHandle rg_visible_pages,
			RGHandle rg_current_hiz,
			const GPUScene& gpu_scene);
		RGHandle add_indirect_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle depth_buffer,
			const SceneView& view,
			const RenderConfig& config,
			const RenderScene& render_scene,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list,
			const SceneDrawRanges& ranges,
			RGHandle rg_draw,
			RGHandle rg_instance_data,
			const GPUScene& gpu_scene,
			BufferHandle mega_vertex_buffer,
			BufferHandle mega_index_buffer,
			RGHandle* out_depth = nullptr);
	};

	class ScreenSpaceReflectionPass : public RenderPass {
		PipelineHandle ssr_pipeline;

	public:
		~ScreenSpaceReflectionPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle depth_buffer, RGHandle scene_color,
			const SceneView& view, const RenderConfig& config);
	};

	class ScreenSpaceGlobalIlluminationPass : public RenderPass {
		PipelineHandle ssgi_pipeline;
		PipelineHandle denoise_pipeline;
		PipelineHandle temporal_pipeline;

		TextureHandle history_textures[2];
		uint32_t history_read_index = 0;
		bool has_valid_history = false;
		uint32_t history_width = 0;
		uint32_t history_height = 0;
		bud::math::mat4 last_view_proj = bud::math::mat4(1.0f);
		bud::math::mat4 last_view = bud::math::mat4(1.0f);
		bool has_last_view = false;
		RHI* stored_rhi = nullptr;

	public:
		~ScreenSpaceGlobalIlluminationPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle depth_buffer, RGHandle scene_color,
			const SceneView& view, const RenderConfig& config);
	};

class ResolvePass : public RenderPass {
		PipelineHandle resolve_pipeline;
		uint64_t resolve_set_layout = 0;
		uint64_t resolve_descriptor_set = 0;

	public:
		~ResolvePass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle visibility_buffer,
			const SceneView& view,
			const RenderConfig& config,
			const GPUScene& gpu_scene,
			RGHandle shadow_map = {},
			RGHandle ao_map = {},
			RGHandle ssr_map = {},
			RGHandle ssgi_map = {});
	};

	struct PhysicsDebugVertex {
		float pos[3];
		float color[3];
	};

	class PhysicsDebugPass : public RenderPass {
		PipelineHandle pipeline;
		uint64_t set_layout = 0;
		uint64_t descriptor_set = 0;
		BufferHandle vertex_buffer;
		BufferHandle ubo_buffer;
		uint64_t vertex_capacity = 0;
		RGHandle ubo_handle;
		std::vector<PhysicsDebugVertex> cpu_vertices;
		// cpu_vertices is written by the logic thread (Renderer::update_physics_debug_vertices)
		// and consumed by the render thread (add_to_graph), so the handoff must be locked.
		std::mutex vertices_mutex;
		bool has_ubo = false;

	public:
		~PhysicsDebugPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void update_vertices(const std::vector<PhysicsDebugVertex>& verts);
		RGHandle add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle depth_buffer,
			const SceneView& view, const RenderConfig& config);
	};

	class TAAPass : public RenderPass {
		TextureHandle history_textures[2];
		uint32_t history_read_index = 0;
		bool has_valid_history = false;

		RHI* stored_rhi = nullptr;
		uint32_t history_width = 0;
		uint32_t history_height = 0;

	public:
		~TAAPass() = default;
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle scene_color, RGHandle depth_buffer,
			const SceneView& view, const RenderConfig& config);
	};

}
