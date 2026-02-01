module;
#include <memory>
#include <vector>

export module bud.graphics.renderer;

import bud.io;
import bud.math;
import bud.scene;

import bud.graphics.types;
import bud.graphics.rhi;
import bud.graphics.graph;
import bud.graphics.passes;

export namespace bud::graphics {
	class Renderer {
	public:
		Renderer(RHI* rhi);
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
