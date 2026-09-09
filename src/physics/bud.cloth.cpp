#include "src/physics/bud.cloth.hpp"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <format>
#include <cctype>

#include "src/io/bud.io.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"

namespace bud::physics {

	namespace {

		std::string to_lower_ascii(std::string_view text) {
			std::string lower;
			lower.reserve(text.size());
			for (char c : text)
				lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			return lower;
		}

		bool contains(std::string_view haystack, std::string_view needle) {
			return haystack.find(needle) != std::string_view::npos;
		}

		bool is_banner_name(std::string_view lower_name) {
			if (contains(lower_name, "banner"))
				return true;
			// Sponza hanging banners: sponza_282 .. sponza_289
			for (int i = 282; i <= 289; ++i) {
				if (contains(lower_name, "sponza_" + std::to_string(i)))
					return true;
			}
			return false;
		}

		void destroy_world_buffers(bud::graphics::RHI* rhi, ClothSimWorld& world) {
			if (!rhi)
				return;
			if (world.gpu_particles.is_valid()) {
				rhi->destroy_buffer(world.gpu_particles);
				world.gpu_particles.reset();
			}
			if (world.gpu_rest_particles.is_valid()) {
				rhi->destroy_buffer(world.gpu_rest_particles);
				world.gpu_rest_particles.reset();
			}
			if (world.gpu_constraints.is_valid()) {
				rhi->destroy_buffer(world.gpu_constraints);
				world.gpu_constraints.reset();
			}
			if (world.gpu_lambdas.is_valid()) {
				rhi->destroy_buffer(world.gpu_lambdas);
				world.gpu_lambdas.reset();
			}
			if (world.gpu_bindings.is_valid()) {
				rhi->destroy_buffer(world.gpu_bindings);
				world.gpu_bindings.reset();
			}
			if (world.gpu_cell_heads.is_valid()) {
				rhi->destroy_buffer(world.gpu_cell_heads);
				world.gpu_cell_heads.reset();
			}
			if (world.gpu_particle_next.is_valid()) {
				rhi->destroy_buffer(world.gpu_particle_next);
				world.gpu_particle_next.reset();
			}
		}

		// Upload helper: staging copy + immediate submit.
		template <typename T>
		void upload_buffer(bud::graphics::RHI* rhi, const std::vector<T>& data, bud::graphics::BufferHandle& dst) {
			const uint64_t bytes = static_cast<uint64_t>(data.size()) * sizeof(T);
			auto staging = rhi->create_upload_buffer(bytes);
			if (auto* buf = rhi->get_buffer(staging); buf && buf->mapped_ptr)
				std::memcpy(buf->mapped_ptr, data.data(), bytes);
			rhi->copy_buffer_immediate(staging, dst, bytes);
			rhi->destroy_buffer(staging);
		}

		// Auto-pin particles along the hanging rod (top of the cloth, local space).
		// Banners are anchored at both gallery endpoints, curtains along the top rod.
		void patch_pins(ClothInstanceCPU& inst) {
			const uint32_t count = static_cast<uint32_t>(inst.particles.size());
			if (count == 0)
				return;

			float y_min = 1e30f, y_max = -1e30f;
			float x_min = 1e30f, x_max = -1e30f;
			float z_min = 1e30f, z_max = -1e30f;
			for (const auto& p : inst.particles) {
				const auto& pos = p.position_inv_mass;
				y_min = std::min(y_min, pos.y);
				y_max = std::max(y_max, pos.y);
				x_min = std::min(x_min, pos.x);
				x_max = std::max(x_max, pos.x);
				z_min = std::min(z_min, pos.z);
				z_max = std::max(z_max, pos.z);
			}

			inst.min_p = bud::math::vec3(x_min, y_min, z_min);
			inst.max_p = bud::math::vec3(x_max, y_max, z_max);
			inst.center = (inst.min_p + inst.max_p) * 0.5f;

			const float height_span = std::max(y_max - y_min, 0.01f);
			const float x_span = x_max - x_min;
			const float z_span = z_max - z_min;
			const bool primary_x = (x_span >= z_span);
			const float width_min = primary_x ? x_min : z_min;
			const float width_max = primary_x ? x_max : z_max;
			const float width_span = std::max(width_max - width_min, 0.01f);

			constexpr float bin_size = 0.05f;
			const size_t num_bins = std::max<size_t>(1, static_cast<size_t>(std::ceil(width_span / bin_size)));
			std::vector<float> bin_max_y(num_bins, -1e30f);
			for (const auto& p : inst.particles) {
				const float u = primary_x ? p.position_inv_mass.x : p.position_inv_mass.z;
				const size_t bin = std::clamp<size_t>(static_cast<size_t>((u - width_min) / bin_size), 0, num_bins - 1);
				bin_max_y[bin] = std::max(bin_max_y[bin], p.position_inv_mass.y);
			}

			const bool is_banner = is_banner_name(to_lower_ascii(inst.asset_path));
			uint32_t pinned = 0;
			if (is_banner) {
				// Decorative swag banners are sewn to their support cord along the
				// ENTIRE top edge (which follows the artist-authored swag curve),
				// not just at the two pillar endpoints. Pinning only the endpoints
				// lets the middle span collapse into a deep catenary under gravity.
				// The lower hem stays free and keeps reacting to wind.
				for (auto& p : inst.particles) {
					auto& pos = p.position_inv_mass;
					const float u = primary_x ? pos.x : pos.z;
					const size_t bin = std::clamp<size_t>(static_cast<size_t>((u - width_min) / bin_size), 0, num_bins - 1);
					const float local_top = bin_max_y[bin];
					if (pos.y >= (local_top - 0.15f)) {
						pos.w = 0.0f;
						p.prev_position = pos;
						pinned++;
					}
				}
			}
			else {
				// Curtains / tapestries hung along a horizontal rod at the top.
				const float min_anchor_y = y_min + 0.75f * height_span;
				for (auto& p : inst.particles) {
					auto& pos = p.position_inv_mass;
					const float u = primary_x ? pos.x : pos.z;
					const size_t bin = std::clamp<size_t>(static_cast<size_t>((u - width_min) / bin_size), 0, num_bins - 1);
					const float local_top = bin_max_y[bin];
					if (pos.y >= min_anchor_y && pos.y >= (local_top - 0.08f)) {
						pos.w = 0.0f;
						p.prev_position = pos;
						pinned++;
					}
				}
			}

			bud::print("[ClothSystem] Auto-pin '{}': pinned {}/{} particles (banner={})",
				inst.asset_path, pinned, count, is_banner);
		}

	} // namespace

	bool is_cloth_name_or_path(std::string_view name_or_path) {
		const std::string lower = to_lower_ascii(name_or_path);
		if (contains(lower, "cloth") || contains(lower, "curtain") ||
			contains(lower, "fabric") || contains(lower, "banner"))
			return true;

		// Sponza banners: 282..289, curtains: 320..329
		for (int i = 282; i <= 289; ++i) {
			if (contains(lower, "sponza_" + std::to_string(i)))
				return true;
		}
		for (int i = 320; i <= 329; ++i) {
			if (contains(lower, "sponza_" + std::to_string(i)))
				return true;
		}
		return false;
	}

	ClothSystem::~ClothSystem() {
		shutdown();
	}

	void ClothSystem::init(bud::graphics::RHI* rhi, bud::io::AssetManager* asset_manager, bud::graphics::GPUScene* gpu_scene) {
		stored_rhi = rhi;
		stored_asset_manager = asset_manager;
		stored_gpu_scene = gpu_scene;
		load_pipelines();
	}

	void ClothSystem::shutdown() {
		std::lock_guard lock(state_mutex);
		if (stored_rhi) {
			if (active_world) {
				destroy_world_buffers(stored_rhi, *active_world);
				active_world.reset();
			}
			for (auto& retired : retired_worlds) {
				if (retired.world) {
					destroy_world_buffers(stored_rhi, *retired.world);
					retired.world.reset();
				}
			}
			retired_worlds.clear();
			if (pipeline_integrate.is_valid()) {
				stored_rhi->destroy_pipeline(pipeline_integrate);
				pipeline_integrate.reset();
			}
			if (pipeline_solver.is_valid()) {
				stored_rhi->destroy_pipeline(pipeline_solver);
				pipeline_solver.reset();
			}
			if (pipeline_skinning.is_valid()) {
				stored_rhi->destroy_pipeline(pipeline_skinning);
				pipeline_skinning.reset();
			}
		}
		instances.clear();
		world_dirty = false;
		has_world.store(false, std::memory_order_release);
		pipelines_loaded = false;
	}

	void ClothSystem::load_pipelines() {
		if (!stored_rhi || !stored_asset_manager)
			return;

		// The asset manager fires callbacks on the main thread, possibly after this
		// ClothSystem has been destroyed. Guard every callback with the token.
		std::weak_ptr<int> alive = alive_token;

		stored_asset_manager->load_file_async("src/shaders/cloth_integrate.comp.spv", [this, alive](std::vector<char> data) {
			if (alive.expired() || data.empty())
				return;
			bud::graphics::ComputePipelineDesc desc;
			desc.cs.code = std::move(data);
			desc.layout_kind = bud::graphics::ComputePipelineDesc::LayoutKind::ClothIntegrate;
			pipeline_integrate = stored_rhi->create_compute_pipeline(desc);
		});

		stored_asset_manager->load_file_async("src/shaders/cloth_solver.comp.spv", [this, alive](std::vector<char> data) {
			if (alive.expired() || data.empty())
				return;
			bud::graphics::ComputePipelineDesc desc;
			desc.cs.code = std::move(data);
			desc.layout_kind = bud::graphics::ComputePipelineDesc::LayoutKind::ClothSolver;
			pipeline_solver = stored_rhi->create_compute_pipeline(desc);
		});

		stored_asset_manager->load_file_async("src/shaders/cloth_skinning.comp.spv", [this, alive](std::vector<char> data) {
			if (alive.expired() || data.empty())
				return;
			bud::graphics::ComputePipelineDesc desc;
			desc.cs.code = std::move(data);
			desc.layout_kind = bud::graphics::ComputePipelineDesc::LayoutKind::ClothSkinning;
			pipeline_skinning = stored_rhi->create_compute_pipeline(desc);
		});

		pipelines_loaded = true;
	}

	void ClothSystem::register_cloth_asset(const std::string& asset_path, uint32_t mesh_id, int32_t vertex_offset, const std::vector<char>& chunk_data) {
		if (!stored_rhi || chunk_data.size() < sizeof(bud::asset::ClothPhysicsChunkHeader))
			return;

		const auto* header = reinterpret_cast<const bud::asset::ClothPhysicsChunkHeader*>(chunk_data.data());
		if (header->magic != bud::asset::CLOTH_PHYSICS_MAGIC) {
			bud::eprint("[ClothSystem] Invalid ClothPhysics magic 0x{:X} for '{}'", header->magic, asset_path);
			return;
		}
		if (header->lod_count == 0)
			return;

		// One physics mesh per cloth: only LOD0 is simulated. LOD1 stays in the asset
		// for future use but is never uploaded or stepped.
		const auto& lod_hdr = header->lods[0];
		if (lod_hdr.sim_particle_count == 0)
			return;

		const uint64_t p_size = static_cast<uint64_t>(lod_hdr.sim_particle_count) * sizeof(SimParticle);
		const uint64_t c_size = static_cast<uint64_t>(lod_hdr.constraint_count) * sizeof(DistanceConstraint);
		const uint64_t b_size = static_cast<uint64_t>(lod_hdr.binding_count) * sizeof(ClothSkinBinding);
		if (static_cast<uint64_t>(lod_hdr.sim_particle_offset) + p_size > chunk_data.size() ||
			static_cast<uint64_t>(lod_hdr.constraint_offset) + c_size > chunk_data.size() ||
			static_cast<uint64_t>(lod_hdr.binding_offset) + b_size > chunk_data.size()) {
			bud::eprint("[ClothSystem] Chunk data out of bounds for LOD0 of '{}'", asset_path);
			return;
		}

		{
			std::lock_guard lock(state_mutex);
			for (const auto& inst : instances) {
				if (inst.mesh_id == mesh_id)
					return; // already registered
			}
		}

		ClothInstanceCPU inst;
		inst.asset_path = asset_path;
		inst.mesh_id = mesh_id;
		inst.vertex_offset = vertex_offset;

		inst.particles.resize(lod_hdr.sim_particle_count);
		std::memcpy(inst.particles.data(), chunk_data.data() + lod_hdr.sim_particle_offset, p_size);
		patch_pins(inst);

		// Baked inverse masses are 1.0 (1 kg per particle) - absurdly heavy for fabric.
		// XPBD honors physical compliance (stretch = compliance x tension), so heavy
		// particles made hanging cloth sag meters under gravity, while wind drag
		// acceleration (proportional to inv_mass) could barely move it. Real fabric is
		// tens of grams per particle: scale free inverse masses up -> less gravity sag
		// AND stronger wind response. Pinned particles (w = 0) are untouched.
		constexpr float kClothParticleMassKg = 0.05f;
		const float inv_mass_scale = 1.0f / kClothParticleMassKg;
		for (auto& p : inst.particles) {
			if (p.position_inv_mass.w > 0.0f)
				p.position_inv_mass.w *= inv_mass_scale;
		}

		inst.constraints.resize(lod_hdr.constraint_count);
		if (c_size > 0)
			std::memcpy(inst.constraints.data(), chunk_data.data() + lod_hdr.constraint_offset, c_size);

		inst.bindings.resize(lod_hdr.binding_count);
		if (b_size > 0)
			std::memcpy(inst.bindings.data(), chunk_data.data() + lod_hdr.binding_offset, b_size);

		// Defensive validation: corrupt / mismatched chunks must be rejected here
		// with a clear error instead of crashing the world build later.
		const uint32_t particle_count = lod_hdr.sim_particle_count;
		for (size_t i = 0; i < inst.constraints.size(); ++i) {
			const auto& c = inst.constraints[i];
			if (c.p1 >= particle_count || c.p2 >= particle_count) {
				bud::eprint("[ClothSystem] '{}' LOD0 constraint {} references particle {}/{} >= {} particles, rejecting asset",
					asset_path, i, c.p1, c.p2, particle_count);
				return;
			}
		}
		for (size_t i = 0; i < inst.bindings.size(); ++i) {
			const auto& b = inst.bindings[i];
			if (b.sim_tri_idx[0] >= particle_count || b.sim_tri_idx[1] >= particle_count || b.sim_tri_idx[2] >= particle_count) {
				bud::eprint("[ClothSystem] '{}' LOD0 binding {} references sim particle {}/{}/{} >= {} particles, rejecting asset",
					asset_path, i, b.sim_tri_idx[0], b.sim_tri_idx[1], b.sim_tri_idx[2], particle_count);
				return;
			}
		}

		{
			std::lock_guard lock(state_mutex);
			instances.push_back(std::move(inst));
			world_dirty = true;
		}

		bud::print("[ClothSystem] Registered cloth '{}' (mesh_id={}): {} particles, {} constraints, {} bindings",
			asset_path, mesh_id, lod_hdr.sim_particle_count, lod_hdr.constraint_count, lod_hdr.binding_count);
	}

	std::shared_ptr<const ClothSimWorld> ClothSystem::acquire_world() {
		std::lock_guard lock(state_mutex);
		return active_world;
	}

	void ClothSystem::set_camera_sphere(const bud::math::vec3& center, float radius) {
		std::lock_guard lock(state_mutex);
		camera_sphere_state = bud::math::vec4(center, radius);
	}

	void ClothSystem::set_capsule_collider(const CapsuleCollider& collider, bool enabled) {		std::lock_guard lock(state_mutex);
		current_capsule = collider;
		capsule_enabled = enabled;
	}

	void ClothSystem::set_scene_colliders(std::vector<BoxCollider> colliders) {
		std::lock_guard lock(state_mutex);
		scene_colliders = std::move(colliders);
		world_dirty = true; // column selection happens at world build time
	}

	void ClothSystem::retire_world(std::shared_ptr<ClothSimWorld> world, uint64_t current_frame) {
		if (!world)
			return;
		retired_worlds.push_back({ std::move(world), current_frame });
	}

	void ClothSystem::sweep_retired_worlds(uint64_t current_frame) {
		// A world's buffers may still be referenced by in-flight command buffers from
		// previous frames; keep the last few frames around before destroying (GPU side).
		std::vector<std::shared_ptr<ClothSimWorld>> doomed;
		{
			std::lock_guard lock(state_mutex);
			const uint64_t cutoff = (current_frame >= 4) ? (current_frame - 4) : 0;
			auto it = retired_worlds.begin();
			while (it != retired_worlds.end()) {
				if (it->frame_stamp <= cutoff) {
					doomed.push_back(std::move(it->world));
					it = retired_worlds.erase(it);
				}
				else {
					++it;
				}
			}
		}
		if (stored_rhi) {
			for (auto& w : doomed) {
				destroy_world_buffers(stored_rhi, *w);
			}
		}
	}

	void ClothSystem::select_columns(ClothSimWorld& world) {
		world.column_count = 0;
		const size_t num_colliders = scene_colliders.size();
		if (num_colliders == 0 || instances.empty())
			return;

		constexpr float max_column_dist_sq = 4.0f; // within 2m of a cloth anchor
		size_t chosen[4] = { SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX };
		float best_dist[4] = { 1e30f, 1e30f, 1e30f, 1e30f };

		auto try_add = [&](size_t collider_idx, float dist_sq) {
			// Already selected?
			for (uint32_t c = 0; c < 4; ++c) {
				if (chosen[c] == collider_idx) {
					best_dist[c] = std::min(best_dist[c], dist_sq);
					return;
				}
			}
			// Insert into the worst slot if closer
			uint32_t worst = 0;
			for (uint32_t c = 1; c < 4; ++c) {
				if (best_dist[c] > best_dist[worst])
					worst = c;
			}
			if (dist_sq < best_dist[worst]) {
				chosen[worst] = collider_idx;
				best_dist[worst] = dist_sq;
			}
		};

		for (const auto& inst : instances) {
			const float span_x = inst.max_p.x - inst.min_p.x;
			const float span_z = inst.max_p.z - inst.min_p.z;
			const bud::math::vec3 left_anchor = (span_x >= span_z)
				? bud::math::vec3(inst.min_p.x, inst.center.y, inst.center.z)
				: bud::math::vec3(inst.center.x, inst.center.y, inst.min_p.z);
			const bud::math::vec3 right_anchor = (span_x >= span_z)
				? bud::math::vec3(inst.max_p.x, inst.center.y, inst.center.z)
				: bud::math::vec3(inst.center.x, inst.center.y, inst.max_p.z);

			for (size_t i = 0; i < num_colliders; ++i) {
				const auto& col = scene_colliders[i];
				// Must be a slender vertical column: height >= 1.6m, cross-section <= 0.45m.
				// Strictly reject giant architecture walls, floors and platforms.
				if (col.half_extents.y < 0.8f)
					continue;
				if (col.half_extents.x > 0.45f || col.half_extents.z > 0.45f)
					continue;

				const bud::math::vec2 d_left = bud::math::vec2(left_anchor.x, left_anchor.z) - bud::math::vec2(col.center.x, col.center.z);
				const bud::math::vec2 d_right = bud::math::vec2(right_anchor.x, right_anchor.z) - bud::math::vec2(col.center.x, col.center.z);
				const float dist_l = bud::math::dot(d_left, d_left);
				const float dist_r = bud::math::dot(d_right, d_right);
				if (dist_l < max_column_dist_sq)
					try_add(i, dist_l);
				if (dist_r < max_column_dist_sq)
					try_add(i, dist_r);
			}
		}

		for (uint32_t c = 0; c < 4; ++c) {
			if (chosen[c] == SIZE_MAX)
				continue;
			world.columns[world.column_count++] = scene_colliders[chosen[c]];
		}
	}

	std::shared_ptr<ClothSimWorld> ClothSystem::build_world() {
		// Main thread only (called from flush_pending). instances is main-thread data.
		auto world = std::make_shared<ClothSimWorld>();
		world->particle_count = 0;
		world->constraint_count = 0;
		world->binding_count = 0;
		for (const auto& inst : instances) {
			if (inst.vertex_offset < 0)
				continue;
			world->particle_count += static_cast<uint32_t>(inst.particles.size());
			world->constraint_count += static_cast<uint32_t>(inst.constraints.size());
			world->binding_count += static_cast<uint32_t>(inst.bindings.size());
		}
		if (world->particle_count == 0) {
			// Keep dirty when some instances still wait for their vertex offset.
			for (const auto& inst : instances) {
				if (inst.vertex_offset < 0) {
					world_dirty = true;
					break;
				}
			}
			return nullptr;
		}

		// 1. Concatenate all instances into global pools.
		std::vector<SimParticle> particles(world->particle_count);
		std::vector<SimParticle> rest(world->particle_count);
		std::vector<DistanceConstraint> constraints;
		constraints.reserve(world->constraint_count);
		std::vector<ClothSkinBinding> bindings;
		bindings.reserve(world->binding_count);

		uint32_t pbase = 0;
		for (const auto& inst : instances) {
			if (inst.vertex_offset < 0)
				continue;

			// Base of this instance's particle range in the global pool: constraint
			// and binding indices are local to the instance and must be offset by
			// this base (captured BEFORE the particle loop advances pbase).
			const uint32_t inst_base = pbase;

			for (const auto& p : inst.particles) {
				particles[pbase] = p;
				rest[pbase] = p;
				rest[pbase].position_inv_mass.w = inst.max_p.y; // per-particle top_y for wind attenuation
				pbase++;
			}
			for (auto c : inst.constraints) {
				c.p1 += inst_base;
				c.p2 += inst_base;
				constraints.push_back(c);
			}
			for (auto b : inst.bindings) {
				b.sim_tri_idx[0] += inst_base;
				b.sim_tri_idx[1] += inst_base;
				b.sim_tri_idx[2] += inst_base;
				b.render_vertex_idx = static_cast<uint32_t>(inst.vertex_offset) + b.render_vertex_idx;
				bindings.push_back(b);
			}
		}

		// 2. Proper greedy graph coloring: per-particle set of used colors guarantees
		// that no two constraints inside one batch touch the same particle, so a batch
		// dispatch has zero write conflicts (true Gauss-Seidel).
		std::vector<std::vector<uint32_t>> used_colors(world->particle_count);
		std::vector<std::vector<DistanceConstraint>> color_buckets;
		for (const auto& c : constraints) {
			uint32_t chosen = 0;
			for (;; ++chosen) {
				const auto& u1 = used_colors[c.p1];
				const auto& u2 = used_colors[c.p2];
				const bool c1 = std::find(u1.begin(), u1.end(), chosen) != u1.end();
				const bool c2 = std::find(u2.begin(), u2.end(), chosen) != u2.end();
				if (!c1 && !c2)
					break;
			}
			if (chosen >= color_buckets.size())
				color_buckets.emplace_back();
			color_buckets[chosen].push_back(c);
			used_colors[c.p1].push_back(chosen);
			used_colors[c.p2].push_back(chosen);
		}

		std::vector<DistanceConstraint> sorted_constraints;
		sorted_constraints.reserve(constraints.size());
		world->batches.clear();
		for (const auto& bucket : color_buckets) {
			if (bucket.empty())
				continue;
			ConstraintBatch b;
			b.offset = static_cast<uint32_t>(sorted_constraints.size());
			b.count = static_cast<uint32_t>(bucket.size());
			world->batches.push_back(b);
			sorted_constraints.insert(sorted_constraints.end(), bucket.begin(), bucket.end());
		}

		// 3. Select up to 4 pillar colliders near the cloth anchor points.
		select_columns(*world);

		// 4. Upload GPU buffers.
		world->gpu_particles = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(world->particle_count) * sizeof(SimParticle), bud::graphics::ResourceState::UnorderedAccess);
		world->gpu_rest_particles = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(world->particle_count) * sizeof(SimParticle), bud::graphics::ResourceState::UnorderedAccess);
		world->gpu_lambdas = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(world->constraint_count) * sizeof(float), bud::graphics::ResourceState::UnorderedAccess);
		world->gpu_constraints = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(world->constraint_count) * sizeof(DistanceConstraint), bud::graphics::ResourceState::UnorderedAccess);
		world->gpu_bindings = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(world->binding_count) * sizeof(ClothSkinBinding), bud::graphics::ResourceState::UnorderedAccess);

		upload_buffer(stored_rhi, particles, world->gpu_particles);
		upload_buffer(stored_rhi, rest, world->gpu_rest_particles);
		if (world->constraint_count > 0) {
			upload_buffer(stored_rhi, sorted_constraints, world->gpu_constraints);
		}
		if (world->binding_count > 0) {
			upload_buffer(stored_rhi, bindings, world->gpu_bindings);
		}

		// Spatial hash for self-collision: head table + per-particle next links.
		// Table is a power of two (>= 4 entries per particle) so the shader can
		// mask the hash instead of dividing.
		uint32_t table = 4096u;
		while (table < world->particle_count * 4u && table < (1u << 20u))
			table <<= 1u;
		world->hash_table_size = table;
		world->gpu_cell_heads = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(table) * sizeof(uint32_t), bud::graphics::ResourceState::UnorderedAccess);
		world->gpu_particle_next = stored_rhi->create_gpu_buffer(static_cast<uint64_t>(world->particle_count) * sizeof(uint32_t), bud::graphics::ResourceState::UnorderedAccess);

		bud::print("[ClothSystem] Built SimWorld: {} particles, {} constraints in {} batches, {} bindings, {} columns",
			world->particle_count, world->constraint_count, world->batches.size(), world->binding_count, world->column_count);
		return world;
	}

	void ClothSystem::flush_pending() {
		if (!stored_rhi)
			return;

		bool need = false;
		{
			std::lock_guard lock(state_mutex);
			need = world_dirty;
			// Retry resolving instances that were registered before their geometry
			// landed in the mega buffer (normally resolved at registration time).
			if (need && stored_gpu_scene) {
				for (auto& inst : instances) {
					if (inst.vertex_offset < 0) {
						try {
							inst.vertex_offset = stored_gpu_scene->get_mesh_geometry(inst.mesh_id).vertex_offset;
						}
						catch (...) {
						}
					}
				}
			}
		}
		if (!need)
			return;

		auto world = build_world();
		if (!world)
			return; // stays dirty, retry next frame

		std::lock_guard lock(state_mutex);
		if (active_world)
			retire_world(std::move(active_world), stored_rhi->get_current_frame_index());
		active_world = std::move(world);
		world_dirty = false;
		// Keep dirty when some instances still wait for their vertex offset.
		for (const auto& inst : instances) {
			if (inst.vertex_offset < 0) {
				world_dirty = true;
				break;
			}
		}
		has_world.store(active_world != nullptr, std::memory_order_release);
	}

	void ClothSystem::simulate_and_skin(bud::graphics::RHI* rhi, bud::graphics::CommandHandle cmd,
		bud::graphics::BufferHandle mega_vertex_buffer,
		float dt, const bud::graphics::RenderConfig& config, float current_time,
		const bud::math::vec3& camera_position,
		const bud::graphics::GPUScene* gpu_scene)
	{
		(void)camera_position;
		(void)gpu_scene;

		if (!rhi || !mega_vertex_buffer.is_valid())
			return;
		if (!pipeline_integrate.is_valid() || !pipeline_solver.is_valid() || !pipeline_skinning.is_valid())
			return;
		if (!config.cloth_config.enable_simulation)
			return;

		// Snapshot the world and collider state; the rest of the frame runs lock-free.
		std::shared_ptr<ClothSimWorld> world;
		CapsuleCollider capsule{};
		bool capsule_on = false;
		bud::math::vec4 camera_sphere{ 0.0f };
		{
			std::lock_guard lock(state_mutex);
			world = active_world;
			capsule = current_capsule;
			capsule_on = capsule_enabled;
			camera_sphere = camera_sphere_state;
		}
		if (!world || world->particle_count == 0)
			return;

		sweep_retired_worlds(rhi->get_current_frame_index());

		// Interaction debug: throttled dump of the collider inputs the simulation
		// actually received this frame (capsule enable state, geometry, camera
		// sphere). If walking into cloth prints capsule_on=0 or stale coordinates,
		// the break is in the feeding chain; if the values look right, it is in the
		// solver.
		static uint32_t s_dbg_frame = 0;
		if (++s_dbg_frame % 120u == 1u)
			bud::print("[ClothSystem] sim state: capsule_on={} bottom=({:.2f},{:.2f},{:.2f}) top=({:.2f},{:.2f},{:.2f}) r={:.2f} vel=({:.2f},{:.2f},{:.2f}) | cam_sphere r={:.2f} at ({:.2f},{:.2f},{:.2f})",
				capsule_on,
				capsule.p_bottom.x, capsule.p_bottom.y, capsule.p_bottom.z,
				capsule.p_top.x, capsule.p_top.y, capsule.p_top.z,
				capsule.radius,
				capsule.velocity.x, capsule.velocity.y, capsule.velocity.z,
				camera_sphere.w,
				camera_sphere.x, camera_sphere.y, camera_sphere.z);

		const float safe_dt = std::clamp(dt, 0.001f, 0.0333f);

		rhi->cmd_begin_debug_label(cmd, "GPU XPBD Cloth Simulation & Skinning", 0.35f, 0.75f, 0.95f);

		// 2. XPBD small-steps (Macklin et al. 2019): N substeps, each running one full
		//    Gauss-Seidel pass over the color batches plus collision, with lambda
		//    accumulators reset per substep. This replaces "1 prediction + N iteration
		//    sweeps": iterating a near-rigid constraint network against explicit Verlet
		//    velocities turns every hanging cloth into a ringing oscillator (periodic
		//    vertical bobbing). Equal dispatch cost, drastically better energy behavior,
		//    and material compliance that stays linear in the UI sliders.
		const uint32_t substeps = std::clamp(config.cloth_config.solver_iterations, 1u, 12u);
		const float sub_dt = safe_dt / static_cast<float>(substeps);

		for (uint32_t s = 0; s < substeps; ++s) {
			// 2a. Predict: explicit Verlet + zero-mean wind + drift safety net.
			rhi->cmd_bind_pipeline(cmd, pipeline_integrate);
			rhi->cmd_bind_storage_buffer(cmd, pipeline_integrate, 0, world->gpu_particles);
			rhi->cmd_bind_storage_buffer(cmd, pipeline_integrate, 1, world->gpu_rest_particles);

			ClothPushConstantsIntegrate pc_int{};
			pc_int.gravity_dt = bud::math::vec4(0.0f, -9.81f, 0.0f, sub_dt);
			pc_int.wind_time = bud::math::vec4(config.cloth_config.wind_direction, current_time + static_cast<float>(s) * sub_dt);
			pc_int.particle_count = world->particle_count;
			pc_int.damping = config.cloth_config.damping;
			pc_int.wind_strength = config.cloth_config.wind_strength;
			rhi->cmd_push_constants(cmd, pipeline_integrate, sizeof(ClothPushConstantsIntegrate), &pc_int);
			rhi->cmd_dispatch(cmd, (world->particle_count + 63u) / 64u, 1, 1);
			rhi->resource_barrier(cmd, world->gpu_particles, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);

			// 2b. Reset lambda accumulators, then one full Gauss-Seidel pass.
			if (world->constraint_count > 0) {
				rhi->cmd_bind_pipeline(cmd, pipeline_solver);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 0, world->gpu_particles);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 1, world->gpu_constraints);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 2, world->gpu_lambdas);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 3, world->gpu_cell_heads);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 4, world->gpu_particle_next);

				ClothPushConstantsSolver pc_solve{};
				pc_solve.counts = bud::math::uvec4(0u, world->constraint_count, world->particle_count, world->constraint_count);
				pc_solve.flags = bud::math::uvec4(2u, 0u, 0u, 0u);
				pc_solve.misc = bud::math::vec4(0.0f, sub_dt, 0.0f, 0.0f);
				rhi->cmd_push_constants(cmd, pipeline_solver, sizeof(ClothPushConstantsSolver), &pc_solve);
				rhi->cmd_dispatch(cmd, (world->constraint_count + 63u) / 64u, 1, 1);
				rhi->resource_barrier(cmd, world->gpu_lambdas, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);
				rhi->resource_barrier(cmd, world->gpu_particles, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);

				for (const auto& batch : world->batches) {
					if (batch.count == 0)
						continue;
					pc_solve.counts = bud::math::uvec4(batch.offset, batch.count, world->particle_count, world->constraint_count);
					pc_solve.flags = bud::math::uvec4(0u, capsule_on ? 1u : 0u, 0u, 0u);
					pc_solve.capsule_bottom_radius = bud::math::vec4(capsule.p_bottom, capsule.radius);
					pc_solve.capsule_top_friction = bud::math::vec4(capsule.p_top, capsule.friction);
					pc_solve.capsule_velocity = bud::math::vec4(capsule.velocity, 0.0f);
					pc_solve.misc = bud::math::vec4(0.0f, sub_dt, 0.0f, 0.0f);
					for (uint32_t b = 0; b < world->column_count && b < 4u; ++b) {
						pc_solve.box_center[b] = bud::math::vec4(world->columns[b].center, 0.0f);
						pc_solve.box_extent[b] = bud::math::vec4(world->columns[b].half_extents, 0.0f);
					}
					pc_solve.box_center[0].w = static_cast<float>(world->column_count);

					rhi->cmd_push_constants(cmd, pipeline_solver, sizeof(ClothPushConstantsSolver), &pc_solve);
					rhi->cmd_dispatch(cmd, (batch.count + 63u) / 64u, 1, 1);
					rhi->resource_barrier(cmd, world->gpu_particles, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);
				}
			}

			// 2c. Collision every substep (columns + ground + character capsule + camera sphere).
			{
				rhi->cmd_bind_pipeline(cmd, pipeline_solver);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 0, world->gpu_particles);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 1, world->gpu_constraints);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 2, world->gpu_lambdas);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 3, world->gpu_cell_heads);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 4, world->gpu_particle_next);

				ClothPushConstantsSolver pc_solve{};
				pc_solve.counts = bud::math::uvec4(0u, 0u, world->particle_count, world->constraint_count);
				pc_solve.flags = bud::math::uvec4(1u, capsule_on ? 1u : 0u, 0u, 0u);
				pc_solve.capsule_bottom_radius = bud::math::vec4(capsule.p_bottom, capsule.radius);
				pc_solve.capsule_top_friction = bud::math::vec4(capsule.p_top, capsule.friction);
				pc_solve.capsule_velocity = bud::math::vec4(capsule.velocity, 0.0f);
				pc_solve.misc = bud::math::vec4(0.0f, sub_dt, 0.0f, 0.0f);
				pc_solve.sphere_center_radius = camera_sphere;
				for (uint32_t b = 0; b < world->column_count && b < 4u; ++b) {
					pc_solve.box_center[b] = bud::math::vec4(world->columns[b].center, 0.0f);
					pc_solve.box_extent[b] = bud::math::vec4(world->columns[b].half_extents, 0.0f);
				}
				pc_solve.box_center[0].w = static_cast<float>(world->column_count);

				rhi->cmd_push_constants(cmd, pipeline_solver, sizeof(ClothPushConstantsSolver), &pc_solve);
				rhi->cmd_dispatch(cmd, (world->particle_count + 63u) / 64u, 1, 1);
				rhi->resource_barrier(cmd, world->gpu_particles, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);
			}

			// 2d. Self-collision every substep: rebuild the spatial hash, then relax
			// particle pairs closer than the cloth thickness so the fabric cannot
			// pass through itself when folded.
			{
				rhi->cmd_bind_pipeline(cmd, pipeline_solver);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 0, world->gpu_particles);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 3, world->gpu_cell_heads);
				rhi->cmd_bind_storage_buffer(cmd, pipeline_solver, 4, world->gpu_particle_next);

				ClothPushConstantsSolver pc_solve{};
				pc_solve.counts = bud::math::uvec4(0u, world->hash_table_size, world->particle_count, world->hash_table_size);
				pc_solve.flags = bud::math::uvec4(3u, 0u, 0u, 0u); // clear hash heads
				pc_solve.misc = bud::math::vec4(0.0f, sub_dt, 0.0f, 0.0f);
				rhi->cmd_push_constants(cmd, pipeline_solver, sizeof(ClothPushConstantsSolver), &pc_solve);
				rhi->cmd_dispatch(cmd, (world->hash_table_size + 63u) / 64u, 1, 1);
				rhi->resource_barrier(cmd, world->gpu_cell_heads, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);

				pc_solve.flags = bud::math::uvec4(4u, 0u, 0u, 0u); // scatter particles into the hash
				rhi->cmd_push_constants(cmd, pipeline_solver, sizeof(ClothPushConstantsSolver), &pc_solve);
				rhi->cmd_dispatch(cmd, (world->particle_count + 63u) / 64u, 1, 1);
				rhi->resource_barrier(cmd, world->gpu_cell_heads, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);

				pc_solve.flags = bud::math::uvec4(5u, 0u, 0u, 0u); // self-collision relax
				rhi->cmd_push_constants(cmd, pipeline_solver, sizeof(ClothPushConstantsSolver), &pc_solve);
				rhi->cmd_dispatch(cmd, (world->particle_count + 63u) / 64u, 1, 1);
				rhi->resource_barrier(cmd, world->gpu_particles, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::UnorderedAccess);
			}
		}

		// 3. Barycentric dual-mesh skinning pass: write skinned world-space vertices
		//    (pos + normal) directly into the GPUScene mega vertex buffer.
		if (world->binding_count > 0) {
			rhi->resource_barrier(cmd, mega_vertex_buffer, bud::graphics::ResourceState::VertexBuffer, bud::graphics::ResourceState::UnorderedAccess);
			rhi->cmd_bind_pipeline(cmd, pipeline_skinning);
			rhi->cmd_bind_storage_buffer(cmd, pipeline_skinning, 0, world->gpu_particles);
			rhi->cmd_bind_storage_buffer(cmd, pipeline_skinning, 1, world->gpu_bindings);
			rhi->cmd_bind_storage_buffer(cmd, pipeline_skinning, 2, mega_vertex_buffer);

			ClothPushConstantsSkinning pc_skin{};
			pc_skin.render_vertex_count = world->binding_count;
			pc_skin.vertex_base_offset = 0u; // destination vertex index is baked into each binding
			rhi->cmd_push_constants(cmd, pipeline_skinning, sizeof(ClothPushConstantsSkinning), &pc_skin);
			rhi->cmd_dispatch(cmd, (world->binding_count + 63u) / 64u, 1, 1);
		}

		// Barrier from compute storage write back to vertex input read.
		rhi->resource_barrier(cmd, mega_vertex_buffer, bud::graphics::ResourceState::UnorderedAccess, bud::graphics::ResourceState::VertexBuffer);

		rhi->cmd_end_debug_label(cmd);
	}

} // namespace bud::physics
