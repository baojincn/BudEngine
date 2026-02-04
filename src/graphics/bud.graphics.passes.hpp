#pragma once

#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "src/core/bud.math.hpp"
#include "src/io/bud.io.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"

namespace bud::graphics {

	class CSMShadowPass {
		void* pipeline = nullptr;
		Texture* static_cache_texture = nullptr;
		bud::math::vec3 last_light_dir = bud::math::vec3(0.0f);
		bool cache_initialized = false;
		RHI* stored_rhi = nullptr;

	public:
		struct ShadowData {
			bud::math::mat4 light_space_matrix;
			bud::math::vec4 light_dir;
		};

		void init(RHI* rhi);
		RGHandle add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes);
	};

	
	class MainPass {
		void* pipeline = nullptr;
		
	public:
		void init(RHI* rhi);
		void add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer,
			const RenderScene& render_scene,     // <--- 改了这里
			const SceneView& view,
			const std::vector<RenderMesh>& meshes,
			const std::vector<SortItem>& sort_list, // <--- 新增
			size_t instance_count);
	};
}
