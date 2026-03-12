#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <limits>
#include <imgui.h>

#include "src/graphics/bud.graphics.passes.hpp"

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"

namespace bud::graphics {

	namespace {
		constexpr uint32_t imgui_font_bindless_slot = 999;

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

	void ZPrepass::init(RHI* rhi, const RenderConfig& config) {
		if (!rhi) {
			std::println(stderr, "[ZPrepass] ERROR: RHI is null.");
			return;
		}

		auto vs_code = bud::io::FileSystem::read_binary("src/shaders/zprepass.vert.spv");
		auto fs_code = bud::io::FileSystem::read_binary("src/shaders/zprepass.frag.spv");

		if (!vs_code || !fs_code) {
			if (!vs_code)
				std::println(stderr, "[ZPrepass] Missing shader: src/shaders/zprepass.vert.spv");

			if (!fs_code)
				std::println(stderr, "[ZPrepass] Missing shader: src/shaders/zprepass.frag.spv");

			return;
		}

		GraphicsPipelineDesc desc;
		desc.vs.code = *vs_code;
		desc.fs.code = *fs_code;
		desc.depth_test = true;
		desc.depth_write = false;
		desc.cull_mode = CullMode::None;
		desc.color_attachment_format = TextureFormat::Undefined;
		desc.depth_compare_op = config.reversed_z ? CompareOp::Greater : CompareOp::Less;
		desc.enable_depth_bias = false; // Disable depth bias for Z-prepass pipeline

		pipeline = rhi->create_graphics_pipeline(desc);
	}

	RGHandle ZPrepass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		size_t instance_count) {
		if (!pipeline) {
			std::println(stderr, "[ZPrepass] ERROR: Pipeline is null.");
			return {};
		}

		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
		});

		if (max_scene_count == 0 || sort_list.empty()) {
			std::println(stderr, "[ZPrepass] ERROR: RenderScene or sort list is empty.");
			return {};
		}

		const auto* backbuffer_tex = render_graph.get_texture(backbuffer);
		if (!backbuffer_tex || backbuffer_tex->width == 0 || backbuffer_tex->height == 0) {
			return {};
		}

		const size_t draw_count = std::min({ instance_count, sort_list.size(), max_scene_count });
		uint32_t target_width = backbuffer_tex->width;
		uint32_t target_height = backbuffer_tex->height;

		TextureDesc depth_desc;
		depth_desc.width = target_width;
		depth_desc.height = target_height;
		depth_desc.format = TextureFormat::D32_FLOAT;

		auto depth_h = std::make_shared<RGHandle>();

		return render_graph.add_pass("Z Prepass",
			[=](RGBuilder& builder) {
				*depth_h = builder.create("MainDepth", depth_desc);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *depth_h;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) {
					std::println(stderr, "[ZPrepass] ERROR: Pipeline is null.");
					return;
				}

				RenderPassBeginInfo info;
				info.depth_attachment = render_graph.get_texture(*depth_h);
				info.clear_depth = true;
				info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				uint32_t last_mesh = std::numeric_limits<uint32_t>::max();

				for (size_t i = 0; i < draw_count; ++i) {
					const auto& item = sort_list[i];
					uint32_t idx = item.entity_index;

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

					if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
						const auto& sub = mesh.submeshes[item.submesh_index];
						push_vars.material_id = sub.material_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);
						rhi->cmd_draw_indexed(cmd, sub.index_count, 1, sub.index_start, 0, 0);
					} else {
						push_vars.material_id = material_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);
						rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
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

	void CSMShadowPass::init(RHI* rhi, const RenderConfig& config) {
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
		desc.depth_compare_op = config.reversed_z ? CompareOp::Greater : CompareOp::Less;
		desc.enable_depth_bias = true;

        
        pipeline = rhi->create_graphics_pipeline(desc);
        if (pipeline) {
            std::println("[CSMShadowPass] Pipeline created successfully: {}", (void*)pipeline);
        } else {
            std::println(stderr, "[CSMShadowPass] ERROR: Pipeline creation returned nullptr!");
        }
    }

	RGHandle CSMShadowPass::add_to_graph(RenderGraph& render_graph, const SceneView& view, const RenderConfig& config,
		const RenderScene& render_scene,
		const std::vector<RenderMesh>& meshes,
		std::vector<std::vector<uint32_t>> csm_visible_instances)
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
					[=, csm_vis = csm_visible_instances, &render_graph, &render_scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
						if (!pipeline) return;

						for (uint32_t i = 0; i < cascade_count; ++i) {
							auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
							bud::math::Frustum cascade_view_frustum_dbg;
							cascade_view_frustum_dbg.update(cascade_light_view_proj);

							RenderPassBeginInfo info;
							info.depth_attachment = render_graph.get_texture(static_cache_h);
							info.clear_depth = true;
							info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;
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

							const auto& visible_instances = csm_vis[i];

							size_t _max_count = std::min(visible_instances.size(), max_scene_count);

							for (size_t i = 0; i < _max_count; ++i) {
								size_t idx = visible_instances[i];

								// 1. 检查是否是静态物体, 利用flags
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
			[=, csm_vis = std::move(csm_visible_instances), &render_graph, &render_scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
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
					info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;
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

					const auto& visible_instances = csm_vis[i];
					size_t count = visible_instances.size();

					for (size_t i = 0; i < count; ++i) {
						size_t idx = visible_instances[i];

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

						// Draw
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
	}
	
	void MainPass::init(RHI* rhi, const RenderConfig& config) {
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
		desc.depth_compare_op = config.reversed_z ? CompareOp::Greater : CompareOp::Less;
		desc.enable_depth_bias = false;

        pipeline = rhi->create_graphics_pipeline(desc);
        //std::println("[MainPass] Pipeline created successfully.");
    }

	void MainPass::add_to_graph(RenderGraph& render_graph, RGHandle shadow_map, RGHandle backbuffer, RGHandle depth_buffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
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

		const auto* backbuffer_tex = render_graph.get_texture(backbuffer);
		if (!backbuffer_tex || backbuffer_tex->width == 0 || backbuffer_tex->height == 0) {
			return;
		}

		const size_t draw_count = std::min({ instance_count, sort_list.size(), max_scene_count });

		uint32_t target_width = backbuffer_tex->width;
		uint32_t target_height = backbuffer_tex->height;

		render_graph.add_pass("Main Lighting Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.read(shadow_map, ResourceState::DepthRead);
				builder.write(depth_buffer, ResourceState::DepthWrite);
				return depth_buffer;
			},

			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) {
					std::println(stderr, "[MainPass] ERROR: Pipeline is null.");
					return;
				}

				RenderPassBeginInfo info;
				info.color_attachments.push_back(render_graph.get_texture(backbuffer));
				info.depth_attachment = render_graph.get_texture(depth_buffer);
				info.clear_color = true;
				info.clear_color_value = { 0.5f, 0.5f, 0.5f, 1.0f };
				info.clear_depth = false;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				// Global Set Bindings
				rhi->update_global_shadow_map(render_graph.get_texture(shadow_map));
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				uint32_t last_material = -1;
				uint32_t last_mesh = -1;

				for (size_t i = 0; i < draw_count; ++i) {
					const auto& item = sort_list[i];
					uint32_t idx = item.entity_index;

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

					if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
						const auto& sub = mesh.submeshes[item.submesh_index];
						push_vars.material_id = sub.material_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);
						rhi->cmd_draw_indexed(cmd, sub.index_count, 1, sub.index_start, 0, 0);
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

	UIPass::~UIPass() {}

	void UIPass::shutdown(RHI* rhi) {
		if (font_texture && rhi) {
			auto* pool = rhi->get_resource_pool();
			if (pool) pool->release_texture(font_texture);
			font_texture = nullptr;
		}
		if (current_vertex_buffer_size > 0 && rhi) {
			rhi->destroy_buffer(vertex_buffer);
			current_vertex_buffer_size = 0;
		}
		if (current_index_buffer_size > 0 && rhi) {
			rhi->destroy_buffer(index_buffer);
			current_index_buffer_size = 0;
		}
	}

	void UIPass::init(RHI* rhi, const RenderConfig& config) {
		// Build font texture FIRST to avoid nullptr exceptions in NewFrame later
		ImGuiIO& imgui_io = ImGui::GetIO();
		unsigned char* pixels;
		int width, height;
		imgui_io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

		TextureDesc tex_desc;
		tex_desc.width = width;
		tex_desc.height = height;
		tex_desc.format = TextureFormat::RGBA8_UNORM;
		tex_desc.mips = 1;

		font_texture = rhi->create_texture(tex_desc, pixels, width * height * 4);
		rhi->set_debug_name(font_texture, ObjectType::Texture, "ImGui_Font_Atlas");

		// Register bindless slot for font texture (e.g. index 1)
		font_bindless_index = imgui_font_bindless_slot;
		rhi->update_bindless_texture(font_bindless_index, font_texture);
		imgui_io.Fonts->SetTexID((ImTextureID)(intptr_t)font_bindless_index);

		auto vs_code = bud::io::FileSystem::read_binary("src/shaders/debug_ui.vert.spv");
		auto fs_code = bud::io::FileSystem::read_binary("src/shaders/debug_ui.frag.spv");

		if (!vs_code || !fs_code) {
			std::println(stderr, "[UIPass] Failed to load ImGui shaders!");
			return;
		}

		GraphicsPipelineDesc desc;
		desc.vs.code = *vs_code;
		desc.fs.code = *fs_code;
		desc.depth_test = false;
		desc.depth_write = false;
		desc.cull_mode = CullMode::None;
		desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
		desc.depth_attachment_format = bud::graphics::TextureFormat::Undefined;
		desc.depth_compare_op = CompareOp::Always;
		desc.enable_depth_bias = false;
		desc.is_ui_layout = true;
		desc.blending_enable = true; // Essential for UI font text

		pipeline = rhi->create_graphics_pipeline(desc);
	}

	void UIPass::update_draw_data(ImDrawData* draw_data) {
		UIDrawDataSnapshot ui_draw_data_snapshot;

		if (draw_data && draw_data->CmdListsCount > 0) {
			ui_draw_data_snapshot.display_pos = draw_data->DisplayPos;
			ui_draw_data_snapshot.display_size = draw_data->DisplaySize;
			ui_draw_data_snapshot.framebuffer_scale = draw_data->FramebufferScale;
			ui_draw_data_snapshot.lists.reserve(draw_data->CmdListsCount);

			for (int n = 0; n < draw_data->CmdListsCount; ++n) {
				const ImDrawList* src_list = draw_data->CmdLists[n];
				UIDrawListSnapshot dst_list;
				dst_list.vertices.assign(src_list->VtxBuffer.Data, src_list->VtxBuffer.Data + src_list->VtxBuffer.Size);
				dst_list.indices.reserve(src_list->IdxBuffer.Size);
				dst_list.commands.reserve(src_list->CmdBuffer.Size);

				for (int i = 0; i < src_list->IdxBuffer.Size; ++i) {
					dst_list.indices.push_back(static_cast<uint32_t>(src_list->IdxBuffer.Data[i]));
				}

				for (int cmd_i = 0; cmd_i < src_list->CmdBuffer.Size; ++cmd_i) {
					const ImDrawCmd& src_cmd = src_list->CmdBuffer[cmd_i];
					dst_list.commands.push_back({
						.clip_rect = src_cmd.ClipRect,
						.elem_count = src_cmd.ElemCount,
						.idx_offset = src_cmd.IdxOffset,
						.vtx_offset = src_cmd.VtxOffset,
						.texture_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>((void*)src_cmd.TextureId))
					});
				}

				ui_draw_data_snapshot.lists.push_back(std::move(dst_list));
			}
		}

		std::lock_guard lock(draw_data_mutex);
		cached_draw_data = std::move(ui_draw_data_snapshot);
	}

	void UIPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer) {
		rg.add_pass("UIPass (ImGui)",
			[&](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
			},
			[=, this](RHI* rhi, CommandHandle cmd) {
				UIDrawDataSnapshot draw_data;
				{
					std::lock_guard lock(draw_data_mutex);
					draw_data = cached_draw_data;
				}

				if (!draw_data.has_data()) {
					return;
				}

				if (!pipeline) return;

				// Create or resize buffers
				uint32_t needed_vb_size = draw_data.total_vtx_count() * sizeof(ImDrawVert);
				uint32_t needed_ib_size = draw_data.total_idx_count() * sizeof(uint32_t);

				if (needed_vb_size > current_vertex_buffer_size) {
					if (current_vertex_buffer_size > 0) rhi->destroy_buffer(vertex_buffer);
					current_vertex_buffer_size = needed_vb_size + 4096;
					vertex_buffer = rhi->create_upload_buffer(current_vertex_buffer_size); 
				}

				if (needed_ib_size > current_index_buffer_size) {
					if (current_index_buffer_size > 0) rhi->destroy_buffer(index_buffer);
					current_index_buffer_size = needed_ib_size + 4096;
					index_buffer = rhi->create_upload_buffer(current_index_buffer_size);
				}

				auto* vtx_dst = (ImDrawVert*)vertex_buffer.mapped_ptr;
				auto* idx_dst = (uint32_t*)index_buffer.mapped_ptr;

				for (const auto& cmd_list : draw_data.lists) {
					std::memcpy(vtx_dst, cmd_list.vertices.data(), cmd_list.vertices.size() * sizeof(ImDrawVert));
					vtx_dst += cmd_list.vertices.size();
					std::memcpy(idx_dst, cmd_list.indices.data(), cmd_list.indices.size() * sizeof(uint32_t));
					idx_dst += cmd_list.indices.size();
				}

				RenderPassBeginInfo ui_pass_info;
				ui_pass_info.color_attachments.push_back(rg.get_texture(backbuffer));
				ui_pass_info.clear_color = false;
				ui_pass_info.clear_depth = false;

				rhi->cmd_begin_render_pass(cmd, ui_pass_info);
				rhi->cmd_bind_pipeline(cmd, pipeline);

				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				rhi->cmd_bind_vertex_buffer(cmd, vertex_buffer.internal_handle);
				rhi->cmd_bind_index_buffer(cmd, index_buffer.internal_handle);

				float fb_width = draw_data.display_size.x * draw_data.framebuffer_scale.x;
				float fb_height = draw_data.display_size.y * draw_data.framebuffer_scale.y;

				rhi->cmd_set_viewport(cmd, fb_width, fb_height);

				struct PushConst {
					bud::math::vec2 scale;
					bud::math::vec2 translate;
					uint32_t texture_id;
					uint32_t padding[3];
				} push_const;

				push_const.scale[0] = 2.0f / draw_data.display_size.x;
				push_const.scale[1] = 2.0f / draw_data.display_size.y;
				push_const.translate[0] = -1.0f - draw_data.display_pos.x * push_const.scale[0];
				push_const.translate[1] = -1.0f - draw_data.display_pos.y * push_const.scale[1];
				push_const.texture_id = font_bindless_index;

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConst), &push_const);

				int global_vtx_offset = 0;
				int global_idx_offset = 0;
				ImVec2 clip_off = draw_data.display_pos;
				ImVec2 clip_scale = draw_data.framebuffer_scale;

				for (const auto& cmd_list : draw_data.lists) {
					for (const auto& pcmd : cmd_list.commands) {

						// Setup clip rectangle
						ImVec2 clip_min((pcmd.clip_rect.x - clip_off.x) * clip_scale.x, (pcmd.clip_rect.y - clip_off.y) * clip_scale.y);
						ImVec2 clip_max((pcmd.clip_rect.z - clip_off.x) * clip_scale.x, (pcmd.clip_rect.w - clip_off.y) * clip_scale.y);

						if (clip_min.x < 0.0f) { clip_min.x = 0.0f; }
						if (clip_min.y < 0.0f) { clip_min.y = 0.0f; }
						if (clip_max.x > fb_width) { clip_max.x = fb_width; }
						if (clip_max.y > fb_height) { clip_max.y = fb_height; }
						if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
							continue;

						// Scissor setup
						rhi->cmd_set_scissor(cmd, (int32_t)clip_min.x, (int32_t)clip_min.y, (uint32_t)(clip_max.x - clip_min.x), (uint32_t)(clip_max.y - clip_min.y));

						push_const.texture_id = pcmd.texture_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConst), &push_const);

						// Draw
						rhi->cmd_draw_indexed(cmd, pcmd.elem_count, 1, pcmd.idx_offset + global_idx_offset, pcmd.vtx_offset + global_vtx_offset, 0);
					}
					global_idx_offset += static_cast<int>(cmd_list.indices.size());
					global_vtx_offset += static_cast<int>(cmd_list.vertices.size());
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}
}
