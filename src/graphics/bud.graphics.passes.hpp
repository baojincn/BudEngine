#pragma once

#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "src/core/bud.math.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/io/bud.io.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"

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
		RGHandle add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config, 
			const bud::scene::Scene& scene, const std::vector<RenderMesh>& meshes);
	};

	// --- 2. 主光照 Pass ---
	class MainPass {
		void* pipeline = nullptr;  // Graphics pipeline handle
		
	public:
		void init(RHI* rhi);
		void add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer,
			const bud::scene::Scene& scene, const SceneView& view,
			const std::vector<RenderMesh>& meshes);
	};
}
