#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <limits>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
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

		static MeshAssetHandle invalid() { return { invalid_id, invalid_id }; }
		bool is_valid() const { return mesh_id != invalid_id; }
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

		// Game-thread safe snapshot (CPU-side bounds only)
		std::vector<bud::math::AABB> get_mesh_bounds_snapshot() const;

	private:
		struct UploadQueue {
			std::mutex mutex;
			std::vector<std::function<void()>> commands;
		};

		void update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb);

		RHI* rhi;
		RenderGraph render_graph;
		RenderConfig render_config;
		bud::io::AssetManager* asset_manager;
		bud::threading::TaskScheduler* task_scheduler;

		std::unique_ptr<CSMShadowPass> csm_pass;
		std::unique_ptr<MainPass> main_pass;

		std::vector<RenderMesh> meshes;
		std::vector<bud::math::AABB> mesh_bounds;
		mutable std::mutex mesh_bounds_mutex;

		std::vector<SortItem> sort_list;

		std::atomic<uint32_t> next_bindless_slot{ 1 };
		std::atomic<uint32_t> next_mesh_id{ 0 };

		std::shared_ptr<UploadQueue> upload_queue;
	};
}
