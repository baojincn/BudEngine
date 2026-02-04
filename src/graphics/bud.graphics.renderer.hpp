#pragma once

#include <memory>
#include <vector>
#include <mutex>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.scene.hpp"

#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"

namespace bud::graphics {
	struct MeshAssetHandle {
		uint32_t mesh_id;      // 对应 meshes 数组索引
		uint32_t material_id;  // 对应 bindless 纹理槽位 (通常是 BaseColor)
	};

	class Renderer {
	public:
		Renderer(RHI* rhi, bud::io::AssetManager* asset_manager, bud::threading::TaskScheduler* task_scheduler);
		~Renderer();

		MeshAssetHandle upload_mesh(const bud::io::MeshData& mesh_data);

		// Only work on Rendering Thread
		void flush_upload_queue();

		void render(const bud::graphics::RenderScene& render_scene, SceneView& scene_view);

		void set_config(const RenderConfig& config);
		const RenderConfig& get_config() const;

		inline const std::vector<RenderMesh>& get_meshes() const { return meshes; }

	private:
		void update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb);

		RHI* rhi;
		RenderGraph render_graph;
		RenderConfig render_config;
		bud::io::AssetManager* asset_manager;
		bud::threading::TaskScheduler* task_scheduler;

		// Pass 实例 (用于保存一些持久化数据，如 Pipeline State)
		std::unique_ptr<CSMShadowPass> csm_pass;
		std::unique_ptr<MainPass> main_pass;

		std::vector<RenderMesh> meshes;
		std::atomic<uint32_t> next_bindless_slot{ 1 };

		std::mutex upload_mutex;
		std::vector<std::function<void()>> pending_rhi_commands;
		uint32_t next_mesh_id = 0;
	};
}
