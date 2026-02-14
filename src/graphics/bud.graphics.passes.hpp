#pragma once

#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "src/core/bud.math.hpp"
#include "src/io/bud.io.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"

namespace bud::graphics {

	class CSMShadowPass {
		void* pipeline = nullptr;
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
		void shutdown();

		struct ShadowData {
			bud::math::mat4 light_space_matrix;
			bud::math::vec4 light_dir;
		};

		void init(RHI* rhi, const RenderConfig& config);
		RGHandle add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes);
	};

	
	class MainPass {
		void* pipeline = nullptr;
		
	public:
		void init(RHI* rhi, const RenderConfig& config);
		void add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer,
			const RenderScene& render_scene,
			const SceneView& view,
			const RenderConfig& config,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list,
			size_t instance_count);
	};
}
