#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include "src/core/bud.math.hpp"

namespace bud::physics {

	// 1. SimMesh particle for XPBD physical simulation (32 bytes strictly aligned)
	struct alignas(16) SimParticle {
		bud::math::vec4 position_inv_mass{ 0.0f, 0.0f, 0.0f, 1.0f }; // xyz: position, w: inv_mass (0.0 = fixed pin)
		bud::math::vec4 prev_position{ 0.0f, 0.0f, 0.0f, 0.0f };     // xyz: previous position, w: padding
	};
	static_assert(sizeof(SimParticle) == 32, "SimParticle must be 32 bytes aligned");

	// 2. XPBD distance constraint for SimMesh (16 bytes aligned)
	struct alignas(16) DistanceConstraint {
		uint32_t p1 = 0;          // particle 1 index
		uint32_t p2 = 0;          // particle 2 index
		float rest_length = 0.0f; // resting distance L0
		float compliance = 0.0f;  // XPBD compliance alpha (m/N)
	};
	static_assert(sizeof(DistanceConstraint) == 16, "DistanceConstraint must be 16 bytes aligned");

	// 3. Dual-mesh barycentric skinning binding (32 bytes aligned)
	struct alignas(16) ClothSkinBinding {
		uint32_t sim_tri_idx[3] = { 0, 0, 0 }; // SimMesh triangle 3 particle indices
		uint32_t render_vertex_idx = 0;        // local render vertex index
		bud::math::vec4 barycentric_coords_offset{ 0.0f, 0.0f, 0.0f, 0.0f }; // xyz: (u, v, w), w: normal_offset h
	};
	static_assert(sizeof(ClothSkinBinding) == 32, "ClothSkinBinding must be 32 bytes aligned");

	// 4. Jolt character capsule collider description (48 bytes aligned)
	struct alignas(16) CapsuleCollider {
		bud::math::vec3 p_bottom{ 0.0f, 0.0f, 0.0f }; // bottom sphere center A
		float radius = 0.3f;                           // radius R
		bud::math::vec3 p_top{ 0.0f, 1.0f, 0.0f };    // top sphere center B
		float friction = 0.25f;                        // dynamic friction coefficient mu_k
		bud::math::vec3 velocity{ 0.0f, 0.0f, 0.0f }; // character linear velocity V_body
		float padding = 0.0f;
	};
	static_assert(sizeof(CapsuleCollider) == 48, "CapsuleCollider must be 48 bytes aligned");

	// Push constants for cloth_integrate.comp (strictly 128 bytes)
	struct ClothPushConstantsIntegrate {
		bud::math::vec4 gravity_dt{ 0.0f, -9.81f, 0.0f, 1.0f / 60.0f }; // xyz: gravity, w: dt
		bud::math::vec4 wind_time{ 1.0f, 0.0f, 0.3f, 0.0f };            // xyz: wind_direction, w: time
		uint32_t particle_count = 0;
		float damping = 0.08f;
		float wind_strength = 0.25f;
		float wind_wandering = 0.35f;
		float wind_shadow_intensity = 0.80f;
		float pad1 = 0.0f;
		float pad2 = 0.0f;
		float pad3 = 0.0f;
		bud::math::vec4 column_data[4]{};                                // xyz: center, w: radius (<=0 = inactive)
	};
	static_assert(sizeof(ClothPushConstantsIntegrate) == 128, "ClothPushConstantsIntegrate must be 128 bytes");

	struct BoxCollider {
		bud::math::vec3 center{ 0.0f };
		bud::math::vec3 half_extents{ 0.0f };
	};

	struct ConstraintBatch {
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	// Push constants for cloth_solver.comp (strictly 224 bytes).
	// One global simulation world: all cloth instances share one particle/constraint pool.
	struct ClothPushConstantsSolver {
		bud::math::uvec4 counts{ 0u }; // x: batch_offset, y: batch_count, z: particle_count, w: constraint_total
		bud::math::uvec4 flags{ 0u };  // x: solve_collision_flag (0=batch, 1=collision, 2=clear lambda), y: solve_capsule_flag
		bud::math::vec4 box_center[4]{};
		bud::math::vec4 box_extent[4]{};
		bud::math::vec4 capsule_bottom_radius{ 0.0f, 0.0f, 0.0f, 0.3f }; // xyz: p_bottom, w: radius
		bud::math::vec4 capsule_top_friction{ 0.0f, 1.0f, 0.0f, 0.25f }; // xyz: p_top, w: friction mu_k
		bud::math::vec4 capsule_velocity{ 0.0f };                        // xyz: character velocity, w: unused
		bud::math::vec4 misc{ 0.0f, 0.0166f, 0.0f, 0.0f };               // x: floor_y, y: dt, z: self_friction_static, w: self_friction_kinetic
		bud::math::vec4 sphere_center_radius{ 0.0f };                    // xyz: camera sphere center, w: radius (0 = disabled)
	};
	static_assert(sizeof(ClothPushConstantsSolver) == 240, "ClothPushConstantsSolver must be exactly 240 bytes");

	// Push constants for cloth_skinning.comp
	struct ClothPushConstantsSkinning {
		uint32_t render_vertex_count = 0;
		uint32_t vertex_base_offset = 0; // vertex offset in mega_vertex_buffer
		float padding[2] = { 0.0f, 0.0f };
	};

	enum class ClothPreset : int {
		HeavyTapestry = 0, // Sponza 厚重羊毛挂毯/毛毯 (默认推荐)
		Silk = 1,          // 轻薄真丝
		CottonLinen = 2,   // 棉麻窗帘
		HeavyDenim = 3     // 硬挺牛仔
	};

	struct ClothConfig {
		bool enable_simulation = true;
		bool enable_scene_collision = true;
		ClothPreset preset = ClothPreset::HeavyTapestry;
		uint32_t solver_iterations = 8;
		float damping = 0.07f;
		float wind_strength = 0.85f;
		bud::math::vec3 wind_direction{ 1.0f, 0.0f, 0.3f };
		float warp_compliance = 1e-5f;   // 经向抗拉顺应度 (几乎不可伸长)
		float weft_compliance = 2e-5f;   // 纬向抗拉顺应度
		float shear_compliance = 6e-4f;  // 斜向剪切顺应度 (Trellis 效应产生自然折褶)
		float bend_compliance = 2.5e-3f; // 抗弯顺应度
		float self_friction = 0.35f;     // 自碰撞折叠摩擦力系数 (静摩擦 mu_s = 1.25 * fric, 动摩擦 mu_k = 0.85 * fric)
		float wind_wandering = 0.35f;    // 自然风向动态游弋幅度 (0.0=固定主轴, 1.0=大范围游弋)
		float wind_shadow_intensity = 0.80f; // 建筑物立柱风影遮蔽强度 (0.0=无风影, 1.0=完全遮蔽)
	};

	inline void apply_cloth_preset(ClothConfig& cfg, ClothPreset preset) {
		cfg.preset = preset;
		switch (preset) {
		case ClothPreset::HeavyTapestry:
			cfg.damping = 0.07f;
			cfg.wind_strength = 0.85f;
			cfg.solver_iterations = 8;
			cfg.warp_compliance = 1e-5f;
			cfg.weft_compliance = 2e-5f;
			cfg.shear_compliance = 6e-4f;
			cfg.bend_compliance = 2.5e-3f;
			cfg.self_friction = 0.45f;
			cfg.wind_wandering = 0.35f;
			cfg.wind_shadow_intensity = 0.85f;
			break;
		case ClothPreset::Silk:
			cfg.damping = 0.015f;
			cfg.wind_strength = 1.40f;
			cfg.solver_iterations = 6;
			cfg.warp_compliance = 2e-6f;
			cfg.weft_compliance = 5e-6f;
			cfg.shear_compliance = 2.5e-3f;
			cfg.bend_compliance = 1.0e-2f;
			cfg.self_friction = 0.15f;
			cfg.wind_wandering = 0.50f;
			cfg.wind_shadow_intensity = 0.90f;
			break;
		case ClothPreset::CottonLinen:
			cfg.damping = 0.05f;
			cfg.wind_strength = 1.00f;
			cfg.solver_iterations = 8;
			cfg.warp_compliance = 1e-5f;
			cfg.weft_compliance = 2e-5f;
			cfg.shear_compliance = 1.2e-3f;
			cfg.bend_compliance = 4.0e-3f;
			cfg.self_friction = 0.35f;
			cfg.wind_wandering = 0.35f;
			cfg.wind_shadow_intensity = 0.80f;
			break;
		case ClothPreset::HeavyDenim:
			cfg.damping = 0.12f;
			cfg.wind_strength = 0.60f;
			cfg.solver_iterations = 8;
			cfg.warp_compliance = 5e-6f;
			cfg.weft_compliance = 1e-5f;
			cfg.shear_compliance = 2.0e-4f;
			cfg.bend_compliance = 1.0e-3f;
			cfg.self_friction = 0.55f;
			cfg.wind_wandering = 0.20f;
			cfg.wind_shadow_intensity = 0.70f;
			break;
		default:
			break;
		}
	}

} // namespace bud::physics
