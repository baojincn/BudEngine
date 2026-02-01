#pragma once

#include <memory>
#include <vector>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/runtime/bud.scene.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

namespace bud::graphics {
	class Renderer {
	public:
		Renderer(RHI* rhi, bud::io::AssetManager* asset_manager);
		~Renderer();

		uint32_t upload_mesh(const bud::io::MeshData& mesh_data);
		void render(const bud::scene::Scene& scene, SceneView& scene_view);

		void set_config(const RenderConfig& config);
		const RenderConfig& get_config() const;

	private:
		void update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb);

		RHI* rhi;
		RenderGraph render_graph;
		RenderConfig render_config;
		bud::io::AssetManager* asset_manager;

		// Pass 实例 (用于保存一些持久化数据，如 Pipeline State)
		std::unique_ptr<CSMShadowPass> csm_pass;
		std::unique_ptr<MainPass> main_pass;

		std::vector<RenderMesh> meshes;
	};
}
