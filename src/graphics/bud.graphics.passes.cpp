#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <cmath>
#include <algorithm>

#include "src/graphics/bud.graphics.passes.hpp"

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"

namespace bud::graphics {

	namespace {
		bool mat4_nearly_equal(const bud::math::mat4& a, const bud::math::mat4& b, float eps = 1e-4f) {
			for (int c = 0; c < 4; ++c) {
				for (int r = 0; r < 4; ++r) {
					if (std::abs(a[c][r] - b[c][r]) > eps) {
						return false;
					}
				}
			}
			return true;
		}

		bool shadow_config_equal(const RenderConfig& a, const RenderConfig& b) {
			return a.shadow_map_size == b.shadow_map_size
				&& a.cascade_count == b.cascade_count
				&& std::abs(a.cascade_split_lambda - b.cascade_split_lambda) < 1e-6f
				&& std::abs(a.shadow_near_plane - b.shadow_near_plane) < 1e-6f
				&& std::abs(a.shadow_far_plane - b.shadow_far_plane) < 1e-3f
				&& std::abs(a.shadow_ortho_size - b.shadow_ortho_size) < 1e-3f
				&& std::abs(a.shadow_bias_constant - b.shadow_bias_constant) < 1e-6f
				&& std::abs(a.shadow_bias_slope - b.shadow_bias_slope) < 1e-6f;
		}
	}

	CSMShadowPass::~CSMShadowPass() {
		shutdown();
	}

	void CSMShadowPass::shutdown() {
		if (stored_rhi) {
			auto* pool = stored_rhi->get_resource_pool();
			if (pool && static_cache_texture) {
				pool->release_texture(static_cache_texture);
			}
		}

		static_cache_texture = nullptr;
		cache_initialized = false;
		has_last_view_proj = false;
		has_last_config = false;
	}

    void CSMShadowPass::init(RHI* rhi) {
        if (!rhi) {
            std::println(stderr, "[CSMShadowPass] ERROR: RHI is null.");
            return;
        }

        stored_rhi = rhi;
        auto vs_code = bud::io::FileSystem::read_binary("src/shaders/shadow.vert.spv");
        // Shadow pass theoretically doesn't need frag shader for D32, but we have one
        auto fs_code = bud::io::FileSystem::read_binary("src/shaders/shadow.frag.spv"); 

        if (!vs_code) throw std::runtime_error("[CSMShadowPass] Failed to load shadow.vert.spv");
        if (!fs_code) throw std::runtime_error("[CSMShadowPass] Failed to load shadow.frag.spv");

        GraphicsPipelineDesc desc;
        desc.vs.code = *vs_code;
        desc.fs.code = *fs_code;
        desc.cull_mode = CullMode::Back; // Only render backfaces
        desc.color_attachment_format = TextureFormat::Undefined;

        
        pipeline = rhi->create_graphics_pipeline(desc);
        if (pipeline) {
            std::println("[CSMShadowPass] Pipeline created successfully: {}", (void*)pipeline);
        } else {
            std::println(stderr, "[CSMShadowPass] ERROR: Pipeline creation returned nullptr!");
        }
    }

	RGHandle CSMShadowPass::add_to_graph(RenderGraph& render_graph, const SceneView& view, const RenderConfig& config,
		const RenderScene& render_scene,
		const std::vector<RenderMesh>& meshes)
	{
		if (config.shadow_map_size == 0 || config.cascade_count == 0) {
			std::println(stderr, "[CSMShadowPass] ERROR: Invalid shadow config (size={}, cascades={}).",
				config.shadow_map_size, config.cascade_count);
			return {};
		}

		const uint32_t cascade_count = std::min(config.cascade_count, MAX_CASCADES);
		if (cascade_count != config.cascade_count) {
			std::println(stderr, "[CSMShadowPass] WARNING: cascade_count clamped to MAX_CASCADES ({})", MAX_CASCADES);
		}

		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
		});

		if (max_scene_count == 0) {
			std::println(stderr, "[CSMShadowPass] ERROR: RenderScene arrays are empty.");
			return {};
		}

		TextureDesc desc;
		desc.width = config.shadow_map_size;
		desc.height = config.shadow_map_size;
		desc.format = TextureFormat::D32_FLOAT;
		desc.type = TextureType::Texture2DArray;
		desc.array_layers = cascade_count;

		if (!config.cache_shadows && static_cache_texture) {
			shutdown();
		}

		bool light_changed = bud::math::length(view.light_dir - last_light_dir) > 0.001f;
		bool view_changed = !has_last_view_proj || !mat4_nearly_equal(view.view_proj_matrix, last_view_proj);
		bool config_changed = !has_last_config || !shadow_config_equal(config, last_config);
		bool need_update = !cache_initialized || light_changed || view_changed || config_changed || !config.cache_shadows;

		if (config.cache_shadows) {
			if (light_changed) last_light_dir = view.light_dir;

			if (need_update) {
				last_view_proj = view.view_proj_matrix;
				last_config = config;
				has_last_view_proj = true;
				has_last_config = true;
			}

			bool needs_recreate = !static_cache_texture
				|| static_cache_texture->width != desc.width
				|| static_cache_texture->height != desc.height
				|| static_cache_texture->format != desc.format
				|| static_cache_texture->type != desc.type
				|| static_cache_texture->array_layers != desc.array_layers;

			if (stored_rhi && needs_recreate) {
				if (static_cache_texture) {
					auto* pool = stored_rhi->get_resource_pool();
					if (pool) {
						pool->release_texture(static_cache_texture);
					}
				}
				static_cache_texture = stored_rhi->create_texture(desc, nullptr, 0);
				need_update = true;
			}
		}

		auto shadow_map_h = std::make_shared<RGHandle>();

		// [CSM] 2. Static Cache Update Pass
		RGHandle static_cache_h;
		bool valid_cache = config.cache_shadows && static_cache_texture;

		if (valid_cache) {
			static_cache_h = render_graph.import_texture("StaticShadowCache", static_cache_texture, ResourceState::Undefined);

			if (need_update) {
				render_graph.add_pass("CSM Static Update",
					[&](RGBuilder& builder) {
						builder.write(static_cache_h, ResourceState::DepthWrite);
						return static_cache_h;
					},
					[=, &render_graph, &render_scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
						if (!pipeline) return;

						for (uint32_t i = 0; i < cascade_count; ++i) {
							// ... Viewport, Scissor, Bind Pipeline (保持不变) ...
							auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
							bud::math::Frustum cascade_view_frustum_dbg;
							cascade_view_frustum_dbg.update(cascade_light_view_proj);

							// Setup RenderPass ...
							RenderPassBeginInfo info;
							info.depth_attachment = render_graph.get_texture(static_cache_h);
							info.clear_depth = true;
							info.base_array_layer = i;
							info.layer_count = 1;

							rhi->cmd_begin_render_pass(cmd, info);
							rhi->cmd_bind_pipeline(cmd, pipeline);
							rhi->cmd_set_viewport(cmd, (float)config.shadow_map_size, (float)config.shadow_map_size);
							rhi->cmd_set_scissor(cmd, config.shadow_map_size, config.shadow_map_size);

							rhi->cmd_set_depth_bias(cmd, config.shadow_bias_constant, 0.0f, config.shadow_bias_slope);
							rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

							// PushConsts Setup (light info)...
							struct PushConsts {
								bud::math::mat4 light_view_proj;
								bud::math::mat4 model;
								bud::math::vec4 light_dir;
								uint32_t material_id;
								uint32_t padding[3];
							} push_consts;

							push_consts.light_view_proj = cascade_light_view_proj;
							push_consts.light_dir = bud::math::vec4(bud::math::normalize(view.light_dir), 0.0f);

							// [核心修改] 遍历 RenderScene
							size_t count = std::min(
								render_scene.instance_count.load(std::memory_order_relaxed),
								max_scene_count);

							for (size_t idx = 0; idx < count; ++idx) {

								// 1. 检查是否是静态物体 (利用我们刚加的 flags)
								auto is_static = (render_scene.flags[idx] & 1) != 0;
								if (!is_static) continue; // ONLY STATIC for cache

								auto mesh_id = render_scene.mesh_indices[idx];

								if (mesh_id >= meshes.size()) continue;
								const auto& mesh = meshes[mesh_id];
								if (!mesh.is_valid()) continue;


								// 2. Culling
								// 使用 RenderScene 里的 World Matrix 变换包围体
								const auto& model_matrix = render_scene.world_matrices[idx];
								bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
								if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;

								// 使用 RenderScene 里预计算好的 World AABB (更快！)
								const auto& world_aabb = render_scene.world_aabbs[idx];
								if (!bud::math::intersect_aabb_frustum(world_aabb, cascade_view_frustum_dbg)) continue;

								// 3. Draw
								push_consts.model = model_matrix;
								rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
								rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);

								if (!mesh.submeshes.empty()) {
									for (const auto& sub : mesh.submeshes) {
										// Submesh culling
										auto sub_world_sphere = sub.sphere.transform(model_matrix);
										if (!bud::math::intersect_sphere_frustum(sub_world_sphere, cascade_view_frustum_dbg)) continue;

										auto sub_world_aabb = sub.aabb.transform(model_matrix);
										if (!bud::math::intersect_aabb_frustum(sub_world_aabb, cascade_view_frustum_dbg)) continue;

										push_consts.material_id = sub.material_id;
										rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
										rhi->cmd_draw_indexed(cmd, sub.index_count, 1, sub.index_start, 0, 0);
									}
								}
								else {
									push_consts.material_id = render_scene.material_indices[idx];
									rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
									rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
								}
							}
							rhi->cmd_end_render_pass(cmd);
						}
					}
				);
				cache_initialized = true;
			}
		}

		// Main Shadow Pass (Dynamic + Copy) TODO: dynamic shadows cover everything
		return render_graph.add_pass("CSM Shadow",
			[&, shadow_map_h](RGBuilder& builder) {
				*shadow_map_h = builder.create("CSM ShadowMap", desc);
				builder.write(*shadow_map_h, ResourceState::DepthWrite);
				if (valid_cache)
					builder.read(static_cache_h, ResourceState::DepthRead);

				return *shadow_map_h;
			},
			[=, &render_graph, &render_scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				auto active_map = render_graph.get_texture(*shadow_map_h);
				bool did_copy = false;

				if (valid_cache && cache_initialized) {
					auto static_map = render_graph.get_texture(static_cache_h);
					rhi->resource_barrier(cmd, static_map, ResourceState::DepthRead, ResourceState::TransferSrc);
					rhi->resource_barrier(cmd, active_map, ResourceState::DepthWrite, ResourceState::TransferDst);
					rhi->cmd_copy_image(cmd, static_map, active_map);
					rhi->resource_barrier(cmd, active_map, ResourceState::TransferDst, ResourceState::DepthWrite);
					rhi->resource_barrier(cmd, static_map, ResourceState::TransferSrc, ResourceState::DepthRead);
					did_copy = true;
				}

				for (uint32_t i = 0; i < config.cascade_count; ++i) {
					auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
					bud::math::Frustum cascade_view_frustum_dbg;
					cascade_view_frustum_dbg.update(cascade_light_view_proj);

					RenderPassBeginInfo info;
					info.depth_attachment = active_map;
					info.clear_depth = !did_copy;
					info.base_array_layer = i;
					info.layer_count = 1;

					rhi->cmd_begin_render_pass(cmd, info);
					rhi->cmd_bind_pipeline(cmd, pipeline);
					rhi->cmd_set_viewport(cmd, (float)config.shadow_map_size, (float)config.shadow_map_size);
					rhi->cmd_set_scissor(cmd, config.shadow_map_size, config.shadow_map_size);

					rhi->cmd_set_depth_bias(cmd, config.shadow_bias_constant, 0.0f, config.shadow_bias_slope);
					rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

					struct PushConsts {
						bud::math::mat4 light_view_proj;
						bud::math::mat4 model;
						bud::math::vec4 light_dir;
						uint32_t material_id;
						uint32_t padding[3];
					} push_consts;

					push_consts.light_view_proj = cascade_light_view_proj;
					push_consts.light_dir = bud::math::vec4(bud::math::normalize(view.light_dir), 0.0f);

					size_t count = render_scene.instance_count.load(std::memory_order_relaxed);
					for (size_t idx = 0; idx < count; ++idx) {
						bool is_static = (render_scene.flags[idx] & 1) != 0;
						if (did_copy && is_static) continue;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;


						// Culling
						const auto& model_matrix = render_scene.world_matrices[idx];
						bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
						if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;

						const auto& world_aabb = render_scene.world_aabbs[idx];
						if (!bud::math::intersect_aabb_frustum(world_aabb, cascade_view_frustum_dbg)) continue;

						// Draw
						rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
						rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);

						if (!mesh.submeshes.empty()) {
							for (const auto& sub : mesh.submeshes) {
								// Submesh culling
								auto sub_world_sphere = sub.sphere.transform(model_matrix);
								if (!bud::math::intersect_sphere_frustum(sub_world_sphere, cascade_view_frustum_dbg)) continue;

								auto sub_world_aabb = sub.aabb.transform(model_matrix);
								if (!bud::math::intersect_aabb_frustum(sub_world_aabb, cascade_view_frustum_dbg)) continue;

								push_consts.material_id = sub.material_id;
								rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
								rhi->cmd_draw_indexed(cmd, sub.index_count, 1, sub.index_start, 0, 0);
							}
						}
						else {
							push_consts.material_id = render_scene.material_indices[idx];
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
						}
					}
					rhi->cmd_end_render_pass(cmd);
				}
			}
		);
	}
	
    void MainPass::init(RHI* rhi) {
        if (!rhi) {
            std::println(stderr, "[MainPass] ERROR: RHI is null.");
            return;
        }

        auto vs_code = bud::io::FileSystem::read_binary("src/shaders/main.vert.spv");
        auto fs_code = bud::io::FileSystem::read_binary("src/shaders/main.frag.spv");

        if (!vs_code || !fs_code) {
            std::println(stderr, "[MainPass] Failed to load shaders!");
            return;
        }

        GraphicsPipelineDesc desc;
        desc.vs.code = *vs_code;
        desc.fs.code = *fs_code;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.cull_mode = CullMode::None; // Sponza Cloth (Double Sided)
        desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB; 

        pipeline = rhi->create_graphics_pipeline(desc);
        //std::println("[MainPass] Pipeline created successfully.");
    }

	void MainPass::add_to_graph(RenderGraph& render_graph, RGHandle shadow_map, RGHandle backbuffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		size_t instance_count)
	{
		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
		});

		if (max_scene_count == 0 || sort_list.empty()) {
			std::println(stderr, "[MainPass] ERROR: RenderScene or sort list is empty.");
			return;
		}

		const size_t draw_count = std::min({ instance_count, sort_list.size(), max_scene_count });

		TextureDesc depth_desc;
		depth_desc.width = (uint32_t)view.viewport_width;
		depth_desc.height = (uint32_t)view.viewport_height;
		depth_desc.format = TextureFormat::D32_FLOAT;

		auto depth_h = std::make_shared<RGHandle>();

		render_graph.add_pass("Main Lighting Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.read(shadow_map, ResourceState::DepthRead);
				*depth_h = builder.create("MainDepth", depth_desc);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *depth_h;
			},

			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) {
					std::println(stderr, "[MainPass] ERROR: Pipeline is null.");
					return;
				}

				RenderPassBeginInfo info;
				info.color_attachments.push_back(render_graph.get_texture(backbuffer));
				info.depth_attachment = render_graph.get_texture(*depth_h);
				info.clear_color = true;
				info.clear_color_value = { 0.5f, 0.5f, 0.5f, 1.0f };
				info.clear_depth = true;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, view.viewport_width, view.viewport_height);
				rhi->cmd_set_scissor(cmd, (uint32_t)view.viewport_width, (uint32_t)view.viewport_height);

				// Global Set Bindings
				rhi->update_global_shadow_map(render_graph.get_texture(shadow_map));
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				uint32_t last_material = -1;
				uint32_t last_mesh = -1;

				for (size_t i = 0; i < draw_count; ++i) {
					const auto& item = sort_list[i];
					uint32_t idx = item.entity_index;

					// 从 RenderScene 获取扁平化数据 (SoA)
					// 此时不需要访问 entity 对象，数据都在 RenderScene 的数组里
					uint32_t mesh_id = render_scene.mesh_indices[idx];
					uint32_t material_id = render_scene.material_indices[idx];
					const auto& model_matrix = render_scene.world_matrices[idx];

					if (mesh_id >= meshes.size()) continue;

					const auto& mesh = meshes[mesh_id];

					if (!mesh.is_valid()) continue;


					if (mesh_id != last_mesh) {
						rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
						rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);
						last_mesh = mesh_id;
					}

					struct PushVars {
						bud::math::mat4 model;
						uint32_t material_id;
						uint32_t padding[3];  
					} push_vars;

					push_vars.model = model_matrix;

					if (!mesh.submeshes.empty()) {
						for (const auto& sub : mesh.submeshes) {
							push_vars.material_id = sub.material_id;
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, sub.index_start, 0, 0);
						}
					}
					else {
						push_vars.material_id = material_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);
						rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}
}
