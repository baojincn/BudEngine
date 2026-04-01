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
	class RenderPassBase {
	public:
		virtual ~RenderPassBase() = default;
		virtual void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) = 0;
		virtual void shutdown(RHI* rhi) {}
	};

	class RenderPass : public RenderPassBase {
	protected:
		void* pipeline = nullptr;

		// Helper for asynchronous multi-shader loading
		void load_shaders_async(bud::io::AssetManager* asset_manager, 
							   const std::vector<std::string>& paths, 
							   std::function<void(std::vector<std::vector<char>>)> on_loaded);

	public:
        virtual ~RenderPass() = default;
        bool is_ready() const {
            if (pipeline != nullptr)
                return true;
            return false;
        }
		void shutdown(RHI* rhi) override;
	};

	class HiZCullingPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle instance_buffer, RGHandle indirect_draw_buffer, RGHandle stats_buffer, RGHandle hiz_pyramid, const SceneView& view, size_t instance_count);
	};

	class HiZMipPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const RenderConfig& config);
	};

	class HiZDebugPass : public RenderPass {
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
            bud::graphics::BufferHandle mega_vertex_buffer,
            bud::graphics::BufferHandle mega_index_buffer);
    };


	class CSMShadowPass : public RenderPass {
		Texture* static_cache_texture = nullptr;
		bud::math::vec3 last_light_dir = bud::math::vec3(0.0f);
		bud::math::mat4 last_view_proj = bud::math::mat4(1.0f);
		RenderConfig last_config{};
		bool has_last_view_proj = false;
		bool has_last_config = false;
		bool cache_initialized = false;
		RHI* stored_rhi = nullptr;

	public:
		~CSMShadowPass();
		void shutdown(RHI* rhi) override;

		struct ShadowData {
			bud::math::mat4 light_space_matrix;
			bud::math::vec4 light_dir;
		};

		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		RGHandle add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes, std::vector<std::vector<uint32_t>> csm_visible_instances, bud::graphics::BufferHandle mega_vertex_buffer, bud::graphics::BufferHandle mega_index_buffer);
	};

	
	class MainPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer, RGHandle depth_buffer,
			const RenderScene& render_scene,
			const SceneView& view,
			const RenderConfig& config,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list,
			size_t instance_count,
			bud::graphics::RGHandle indirect_draw_buffer,
			bud::graphics::RGHandle instance_data,
			bud::graphics::BufferHandle mega_vertex_buffer,
			bud::graphics::BufferHandle mega_index_buffer);
	};

	class ClusterVisualizationPass : public RenderPass {
	public:
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle depth_buffer,
			const RenderScene& render_scene,
			const SceneView& view,
			const RenderConfig& config,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list,
			size_t instance_count,
			bud::graphics::RGHandle indirect_draw_buffer,
			bud::graphics::RGHandle instance_data,
			bud::graphics::BufferHandle mega_vertex_buffer,
			bud::graphics::BufferHandle mega_index_buffer);
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
            if (!lists.empty())
                return true;
            return false;
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
		Texture* font_texture = nullptr;

		bud::graphics::BufferHandle vertex_buffer;
		bud::graphics::BufferHandle index_buffer;
		uint32_t current_vertex_buffer_size = 0;
		uint32_t current_index_buffer_size = 0;

		uint32_t font_bindless_index = 0;
		std::mutex draw_data_mutex;
		UIDrawDataSnapshot cached_draw_data;

	public:
		~UIPass();
		void shutdown(RHI* rhi) override;
		void init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) override;
		void update_draw_data(ImDrawData* draw_data);
		void add_to_graph(RenderGraph& rg, RGHandle backbuffer);
	};
}
