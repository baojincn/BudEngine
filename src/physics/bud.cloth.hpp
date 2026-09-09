#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <atomic>

#include "src/core/bud.math.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/physics/bud.cloth.types.hpp"

namespace bud::io {
	class AssetManager;
}

namespace bud::graphics {
	class GPUScene;
}

namespace bud::physics {

	// Checks if an asset name, path, or entity name represents a cloth / curtain
	bool is_cloth_name_or_path(std::string_view name_or_path);

	// Per-instance CPU-side baked data (LOD0 sim mesh only - one physics mesh per cloth).
	struct ClothInstanceCPU {
		std::string asset_path;
		uint32_t mesh_id = 0;
		int32_t vertex_offset = -1;              // destination vertex offset in mega_vertex_buffer (-1 = unresolved)
		bud::math::vec3 center{ 0.0f };
		bud::math::vec3 min_p{ 0.0f };
		bud::math::vec3 max_p{ 0.0f };
		std::vector<SimParticle> particles;      // pin-patched, local indices
		std::vector<DistanceConstraint> constraints; // local particle indices
		std::vector<ClothSkinBinding> bindings;  // local render vertex indices
	};

	// Immutable GPU-ready snapshot of the whole cloth world. All cloth instances are
	// concatenated into one global particle/constraint/binding pool so the frame-start
	// simulation runs in a handful of dispatches. Rebuilt only when instances change.
	struct ClothSimWorld {
		uint32_t particle_count = 0;
		uint32_t constraint_count = 0;
		uint32_t binding_count = 0;

		std::vector<ConstraintBatch> batches;    // proper graph-coloring: no two constraints in a batch share a particle
		BoxCollider columns[4]{};                // up to 4 slender pillar colliders selected at build time
		uint32_t column_count = 0;

		bud::graphics::BufferHandle gpu_particles;
		bud::graphics::BufferHandle gpu_rest_particles; // xyz: rest pose, w: per-particle top_y (wind attenuation)
		bud::graphics::BufferHandle gpu_constraints;    // global particle indices, sorted by color batch
		bud::graphics::BufferHandle gpu_lambdas;        // per-constraint accumulated XPBD lambda
		bud::graphics::BufferHandle gpu_bindings;       // global sim tri indices + global destination vertex index
		bud::graphics::BufferHandle gpu_cell_heads;     // spatial hash table heads (self-collision)
		bud::graphics::BufferHandle gpu_particle_next;  // per-particle hash linked list
		uint32_t hash_table_size = 0;
	};

	class ClothSystem {
	public:
		ClothSystem() = default;
		~ClothSystem();

		void init(bud::graphics::RHI* rhi, bud::io::AssetManager* asset_manager, bud::graphics::GPUScene* gpu_scene);
		void shutdown();

		// Register a baked cloth asset when its ClothPhysics chunk is loaded (main thread).
		void register_cloth_asset(const std::string& asset_path, uint32_t mesh_id, int32_t vertex_offset, const std::vector<char>& chunk_data);

		// Main-thread: rebuild the global SimWorld if instances changed. Cheap no-op otherwise.
		void flush_pending();

		// Returns true if a simulated cloth world is active and pipelines are loaded.
		bool has_cloth() const { return has_world.load(std::memory_order_acquire) && pipelines_loaded; }

		// Render-thread: snapshot of the active SimWorld for GPU-driven debug draws.
		std::shared_ptr<const ClothSimWorld> acquire_world();

		// Update capsule collider state (from CharacterController). Thread-safe.
		void set_capsule_collider(const CapsuleCollider& collider, bool enabled);

		// Update static scene box colliders (from Jolt physics scene). Thread-safe; triggers rebuild.
		void set_scene_colliders(std::vector<BoxCollider> colliders);

		// Camera sphere that pushes hanging cloth aside (FP eye / TP orbit camera).
		// radius = 0 disables it. Thread-safe.
		void set_camera_sphere(const bud::math::vec3& center, float radius);

		// Render-thread: step XPBD simulation and skin render vertices into mega_vertex_buffer.
		void simulate_and_skin(bud::graphics::RHI* rhi, bud::graphics::CommandHandle cmd,
			bud::graphics::BufferHandle mega_vertex_buffer,
			float dt, const bud::graphics::RenderConfig& config, float current_time,
			const bud::math::vec3& camera_position,
			const bud::graphics::GPUScene* gpu_scene = nullptr);

	private:
		bud::graphics::RHI* stored_rhi = nullptr;
		bud::io::AssetManager* stored_asset_manager = nullptr;
		bud::graphics::GPUScene* stored_gpu_scene = nullptr;

		std::mutex state_mutex; // guards everything below except immutable members
		std::vector<ClothInstanceCPU> instances; // main-thread only (registration / flush)
		std::shared_ptr<ClothSimWorld> active_world;
		struct RetiredWorld {
			std::shared_ptr<ClothSimWorld> world;
			uint64_t frame_stamp = 0;
		};
		std::vector<RetiredWorld> retired_worlds;
		bool world_dirty = false;

		CapsuleCollider current_capsule{};
		bool capsule_enabled = false;
		bud::math::vec4 camera_sphere_state{ 0.0f }; // xyz: center, w: radius (0 = disabled)
		std::vector<BoxCollider> scene_colliders;

		std::atomic<bool> has_world{ false };

		bud::graphics::PipelineHandle pipeline_integrate;
		bud::graphics::PipelineHandle pipeline_solver;
		bud::graphics::PipelineHandle pipeline_skinning;

		bool pipelines_loaded = false;

		// Keep this member LAST: pipeline-load callbacks hold a weak_ptr to it and
		// check expiry to detect a destroyed ClothSystem (use-after-free guard).
		std::shared_ptr<int> alive_token = std::make_shared<int>(0);

		void load_pipelines();
		void retire_world(std::shared_ptr<ClothSimWorld> world, uint64_t current_frame);
		void sweep_retired_worlds(uint64_t current_frame);
		std::shared_ptr<ClothSimWorld> build_world();
		void select_columns(ClothSimWorld& world);
	};

} // namespace bud::physics
