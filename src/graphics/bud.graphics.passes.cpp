#include <vector>
#include <iostream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <limits>
#include <imgui.h>
#include <format>
#include <atomic>
#include <chrono>
#include "src/graphics/bud.graphics.passes.hpp"

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"

namespace bud::graphics {

	void RenderPass::load_shaders_async(bud::io::AssetManager* asset_manager,
		const std::vector<std::string>& paths,
		std::function<void(std::vector<std::vector<char>>)> on_loaded) {
		if (paths.empty()) {
			on_loaded({});
			return;
		}

		struct Context {
			std::vector<std::vector<char>> results;
			std::atomic<size_t> loaded_count{ 0 };
			std::function<void(std::vector<std::vector<char>>)> on_loaded;
		};
		auto ctx = std::make_shared<Context>();
		ctx->results.resize(paths.size());
		ctx->on_loaded = std::move(on_loaded);

		for (size_t i = 0; i < paths.size(); ++i) {
			asset_manager->load_file_async(paths[i], [ctx, i, count = paths.size()](std::vector<char> code) {
				ctx->results[i] = std::move(code);
				if (++ctx->loaded_count == count) {
					ctx->on_loaded(ctx->results);
				}
				});
		}
	}

	namespace {
		constexpr uint32_t imgui_font_bindless_slot = 999;
		constexpr uint32_t ao_map_bindless_slot = 998;

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

	RGHandle CSMShadowPass::add_to_graph(RenderGraph& render_graph, const SceneView& view, const RenderConfig& config,
		const RenderScene& render_scene,
		const std::vector<RenderMesh>& meshes,
		std::vector<std::vector<uint32_t>> csm_visible_instances,
		const GPUScene& gpu_scene,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer,
		bud::graphics::RGHandle rg_instance_data,
		size_t instance_count,
		size_t split_index)
	{
		if (config.shadow_map_size == 0 || config.cascade_count == 0) {
			bud::eprint("[CSMShadowPass] ERROR: Invalid shadow config (size={}, cascades={}).",
				config.shadow_map_size, config.cascade_count);
			return {};
		}

		const uint32_t cascade_count = std::min(config.cascade_count, MAX_CASCADES);
		if (cascade_count != config.cascade_count) {
			bud::eprint("[CSMShadowPass] WARNING: cascade_count clamped to MAX_CASCADES ({})", MAX_CASCADES);
		}

		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
			});

		if (max_scene_count == 0) {
			bud::eprint("[CSMShadowPass] ERROR: RenderScene arrays are empty.");
			return {};
		}

		TextureDesc desc;
		desc.width = config.shadow_map_size;
		desc.height = config.shadow_map_size;
		desc.format = bud::graphics::TextureFormat::D32_FLOAT;
		desc.type = TextureType::Texture2DArray;
		desc.array_layers = cascade_count;

		if (!config.cache_shadows && static_cache_texture) {
			shutdown(stored_rhi);
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

		// Whether the static cache texture is available (used by the cull pass
		// to decide if statics can be skipped for the dynamic CSM draw).
		bool valid_cache = config.cache_shadows && static_cache_texture;

		if (config.enable_gpu_driven && csm_cull_pipeline) {
			render_graph.add_pass("CSM Cull",
				[&](RGBuilder& builder) {
					// Async compute: runs on the dedicated compute queue so it can
					// overlap the graphics passes. Its output (csm_indirect_draw)
					// is consumed by CSM Shadow, which the graphics queue waits on
					// via the compute timeline at submission.
					builder.set_async_compute();
					builder.read(rg_instance_data, ResourceState::ShaderResource);
					auto& frame = gpu_scene.get_frame_resources(stored_rhi->get_current_frame_index());
					if (frame.csm_indirect_draw.is_valid()) {
						builder.write(render_graph.import_buffer("CSMIndirect", frame.csm_indirect_draw, ResourceState::UnorderedAccess), ResourceState::UnorderedAccess);
					}
				},
				[=, &gpu_scene, &view, &config](RHI* rhi, CommandHandle cmd) {
					auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
					if (!frame.csm_indirect_draw.is_valid() || instance_count == 0) return;

					// CRITICAL: the cull shader reads the cascade matrices from
					// the global UBO (binding 4). Nothing has updated the UBO yet
					// at this point in the frame, so push the CURRENT frame's
					// view/cascade matrices now -- otherwise the cull runs one
					// frame behind the camera and the shadow map misses casters
					// while moving (black patches / light leaks on rotation).
					rhi->update_global_uniforms(rhi->get_current_image_index(), view);

					rhi->cmd_bind_pipeline(cmd, csm_cull_pipeline);
					rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 0, render_graph.get_buffer(rg_instance_data));
					rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 1, render_graph.get_buffer(rg_instance_data)); // Dummy
					rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 3, frame.csm_indirect_draw); // Dummy
					rhi->cmd_bind_compute_ubo(cmd, csm_cull_pipeline, 4);
					rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 5, frame.csm_indirect_draw); // Dummy

					struct PushConstants {
						uint32_t total_instances;
						uint32_t did_copy;
						uint32_t static_only;
					} pc;
					pc.total_instances = static_cast<uint32_t>(instance_count);

					uint32_t group_x = (static_cast<uint32_t>(instance_count) + 255) / 256;

					// Pass 1: static-only commands -> static cache buffer.
					if (frame.csm_static_indirect_draw.is_valid()) {
						rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 2, frame.csm_static_indirect_draw);
						pc.did_copy = 0;
						pc.static_only = 1;
						rhi->cmd_push_constants(cmd, csm_cull_pipeline, sizeof(PushConstants), &pc);
						rhi->cmd_dispatch(cmd, group_x, 1, 1);
						rhi->resource_barrier(cmd, frame.csm_static_indirect_draw, ResourceState::UnorderedAccess, ResourceState::IndirectArgument);
					}

					// Pass 2: dynamic commands (or all when the static cache is
					// unavailable) -> main CSM indirect buffer.
					rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 2, frame.csm_indirect_draw);
					pc.did_copy = valid_cache ? 1u : 0u;
					pc.static_only = 0;
					rhi->cmd_push_constants(cmd, csm_cull_pipeline, sizeof(PushConstants), &pc);
					rhi->cmd_dispatch(cmd, group_x, 1, 1);

					rhi->resource_barrier(cmd, frame.csm_indirect_draw, ResourceState::UnorderedAccess, ResourceState::IndirectArgument);
				}
			);
		}

		// [CSM] 2. Static Cache Update Pass
		RGHandle static_cache_h;

		if (valid_cache) {
			static_cache_h = render_graph.import_texture("StaticShadowCache", static_cache_texture, ResourceState::Undefined);

			if (need_update) {
				render_graph.add_pass("CSM Static Update",
					[&](RGBuilder& builder) {
						builder.write(static_cache_h, ResourceState::DepthWrite);
						return static_cache_h;
					},
					[=, csm_vis = csm_visible_instances, &render_graph, &render_scene, &meshes, &view, &gpu_scene](RHI* rhi, CommandHandle cmd) {
						if (!pipeline) return;
						auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
						if (frame.csm_instance_models.is_valid()) {
							rhi->update_global_csm_instance_data(frame.csm_instance_models);
						}

						for (uint32_t i = 0; i < cascade_count; ++i) {
							auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
							bud::math::Frustum cascade_view_frustum_dbg;
							cascade_view_frustum_dbg.update(cascade_light_view_proj);

							RenderPassBeginInfo info;
							Texture* static_tex = nullptr;
							try {
								static_tex = render_graph.get_texture(static_cache_h);
							}
							catch (const std::exception& e) {
								bud::eprint("[CSMShadowPass] failed to get static cache texture: {}", e.what());
								return;
							}
							info.depth_attachment = static_tex;
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

							// Bind global Mega-Buffer once per cascade
							// NOTE: For diagnostics we support an alternative binding scheme where we bind
							// the vertex buffer with a byte-offset per-mesh and issue draw calls with
							// vertexOffset=0. This helps detect whether vertexOffset is being
							// interpreted in vertices vs bytes by the driver/pipeline.
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
							static constexpr bool kUseBindVertexByteOffset = true; // A/B test toggle

							struct PushConsts {
								bud::math::mat4 light_view_proj;
								bud::math::mat4 model;
								uint32_t material_id;
								uint32_t use_gpu_driven;
								uint32_t page_slot;
							} push_consts;

							push_consts.light_view_proj = cascade_light_view_proj;
							push_consts.model = bud::math::mat4(1.0f);
							push_consts.material_id = 0;
							push_consts.use_gpu_driven = config.enable_gpu_driven ? 1 : 0;
							push_consts.page_slot = ~0u;

							const auto pp_buf = gpu_scene.get_page_pool_buffer();
							if (config.enable_gpu_driven) {
								auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
								// Static cache: draw ONLY the static casters from
								// csm_static_indirect_draw (written by csm_cull
								// with static_only=1), so dynamic objects are never
								// baked into the cache.
								if (frame.csm_static_indirect_draw.is_valid()) {
									// shadow.vert reads the FULL-scene CSM instance
									// models from binding 6 (dedicated to CSM), so
									// the main pass's binding 3 is never touched.
									rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);

									if (split_index > 0) {
										rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
										rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
										rhi->cmd_draw_indexed_indirect(cmd, frame.csm_static_indirect_draw, i * static_cast<uint32_t>(instance_count) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
									}

									if (split_index < instance_count && pp_buf.is_valid()) {
										rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
										rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
										rhi->cmd_draw_indexed_indirect(cmd, frame.csm_static_indirect_draw, (i * static_cast<uint32_t>(instance_count) + split_index) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(instance_count - split_index), sizeof(bud::graphics::IndirectCommand));
									}

									}
							}
							else {
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
									const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);


									// 2. Culling
									// 使用 RenderScene 里的 World Matrix 变换包围体
									const auto& model_matrix = render_scene.world_matrices[idx];
									bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
									if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;

									// 3. Draw
									push_consts.model = model_matrix;
									const bool is_paged = mesh.is_page_backed && pp_buf.is_valid();
									push_consts.page_slot = is_paged ? mesh.page_index : ~0u;

									uint32_t sub_idx = render_scene.submesh_indices[idx];
									// Page-backed meshes carry one submesh per LOD run; always
									// rasterize the LOD-selected range (else branch), never
									// lock to submesh[0] (their entity submesh_indices=0).
									if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size() && !mesh.is_page_backed) {
										const auto& sub = mesh.submeshes[sub_idx];
										push_consts.material_id = sub.material_id;
										rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
										if (is_paged) {
											rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
											rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
											rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, (uint32_t)idx);
										}
										else {
											rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
											rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
											rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, (uint32_t)idx);
										}
									}
									else {
										push_consts.material_id = render_scene.material_indices[idx];
										rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
										if (is_paged) {
											rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
											rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
											// Rasterize only the selected LOD level into
											// the shadow map; pages carry all LOD levels
											// and drawing the whole page triples the CSM
											// static update cost.
											uint32_t index_start = 0;
											uint32_t index_count = mesh.index_count;
											float lod_dist = bud::math::length(view.camera_position - mesh.sphere.center);
											float focal = view.proj_matrix[1][1] * view.viewport_height * 0.5f;
											uint32_t lod = bud::graphics::select_page_lod(lod_dist, mesh.sphere.radius, focal,
												mesh.lod_error[1], mesh.lod_error[2], config.lod_error_threshold_px);
											if (lod < 3 && mesh.lod_index_count[lod] > 0) {
												index_start = mesh.lod_index_start[lod];
												index_count = mesh.lod_index_count[lod];
											}
											rhi->cmd_draw_indexed(cmd, index_count, 1, mesh_geometry.first_index + index_start, mesh_geometry.vertex_offset, (uint32_t)idx);
										}
										else if (kUseBindVertexByteOffset) {
											auto vb = mega_vertex_buffer;
											vb.offset = static_cast<uint64_t>(mesh_geometry.vertex_offset) * sizeof(bud::io::MeshData::Vertex);
											rhi->cmd_bind_vertex_buffer(cmd, vb);
											rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, 0, (uint32_t)idx);
										}
										else {
											rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
											rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, mesh_geometry.vertex_offset, (uint32_t)idx);
										}
									}
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
			[=, csm_vis = std::move(csm_visible_instances), &render_graph, &render_scene, &meshes, &view, &gpu_scene](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;
				auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
				if (frame.csm_instance_models.is_valid()) {
					rhi->update_global_csm_instance_data(frame.csm_instance_models);
				}

				auto active_map = render_graph.get_texture(*shadow_map_h);
				bool did_copy = false;

				if (valid_cache && cache_initialized) {
					Texture* static_map = nullptr;
					try {
						static_map = render_graph.get_texture(static_cache_h);
					}
					catch (const std::exception& e) {
						bud::eprint("[CSMShadowPass] failed to get static cache texture for copy: {}", e.what());
						static_map = nullptr;
					}
					if (static_map) {
						rhi->resource_barrier(cmd, static_map, ResourceState::DepthRead, ResourceState::TransferSrc);
						rhi->resource_barrier(cmd, active_map, ResourceState::DepthWrite, ResourceState::TransferDst);
						rhi->cmd_copy_image(cmd, static_map, active_map);
						rhi->resource_barrier(cmd, active_map, ResourceState::TransferDst, ResourceState::DepthWrite);
						rhi->resource_barrier(cmd, static_map, ResourceState::TransferSrc, ResourceState::DepthRead);
						did_copy = true;
					}
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

					// Bind global Mega-Buffer once per cascade
					rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
					rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

					struct PushConsts {
						bud::math::mat4 light_view_proj;
						bud::math::mat4 model;
						uint32_t material_id;
						uint32_t use_gpu_driven;
						uint32_t page_slot;
					} push_consts;

					push_consts.light_view_proj = cascade_light_view_proj;
					push_consts.model = bud::math::mat4(1.0f);
					push_consts.material_id = 0;
					push_consts.use_gpu_driven = config.enable_gpu_driven ? 1 : 0;
					push_consts.page_slot = ~0u;

					// Draw a single shadow caster on the CPU path. The GPU-driven
					// path also uses it to fill casters the GPU cull cannot see
					// (they are outside the main camera view), which would
					// otherwise leave light leaks when the camera rotates.
					const auto pp_buf = gpu_scene.get_page_pool_buffer();
					auto draw_occluder = [&](size_t idx, bool skip_cached_static) {
						bool is_static = (render_scene.flags[idx] & 1) != 0;
						if (skip_cached_static && is_static) return;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) return;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) return;

						// Culling
						const auto& model_matrix = render_scene.world_matrices[idx];
						bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
						if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) return;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						// These are CPU-issued draws: make the vertex shader use
						// push_consts.model instead of the GPU instance buffer.
						push_consts.use_gpu_driven = 0;
						push_consts.model = model_matrix;
						push_consts.page_slot = (mesh.is_page_backed && pp_buf.is_valid()) ? mesh.page_index : ~0u;

						// Page-backed meshes live in the GPU page pool, not the
						// mega geometry pool, so rebind the buffers per draw.
						if (mesh.is_page_backed && pp_buf.is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
							rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
						}
						else {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						}

						uint32_t sub_idx = render_scene.submesh_indices[idx];
						// Page-backed meshes always draw the LOD-selected range via the
						// else branch; their entity submesh_indices=0 must not lock them
						// to a single LOD run.
						if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size() && !mesh.is_page_backed) {
							const auto& sub = mesh.submeshes[sub_idx];
							push_consts.material_id = sub.material_id;
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, (uint32_t)idx);
						}
						else {
							push_consts.material_id = render_scene.material_indices[idx];
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, mesh_geometry.vertex_offset, (uint32_t)idx);
						}
					};

					if (config.enable_gpu_driven) {
						auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
						if (frame.csm_indirect_draw.is_valid()) {
							// csm_cull.comp's second dispatch wrote the DYNAMIC
							// commands (statics skipped because the static cache
							// was copied above), plus out-of-view casters. The
							// commands carry gl_InstanceIndex into the full-scene
							// instance data, read from binding 6 (dedicated to
							// CSM), so the main pass's binding 3 is untouched.

							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);

							if (split_index > 0) {
								rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
								rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
								rhi->cmd_draw_indexed_indirect(cmd, frame.csm_indirect_draw, i * static_cast<uint32_t>(instance_count) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
							}

							if (split_index < instance_count && pp_buf.is_valid()) {
								rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
								rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
								rhi->cmd_draw_indexed_indirect(cmd, frame.csm_indirect_draw, (i * static_cast<uint32_t>(instance_count) + split_index) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(instance_count - split_index), sizeof(bud::graphics::IndirectCommand));
							}
						}
					}
					else {
						const auto& visible_instances = csm_vis[i];
						for (size_t k = 0; k < visible_instances.size(); ++k) {
							draw_occluder(visible_instances[k], did_copy);
						}
					}
					rhi->cmd_end_render_pass(cmd);
				}
			}
		);
	}



	void HiZCullingPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("HiZCullingPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/hiz_cull.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) {
				bud::print("[HiZCullingPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle HiZCullingPass::add_to_graph(RenderGraph& render_graph, RGHandle instance_buffer, RGHandle indirect_draw_buffer, RGHandle stats_buffer, RGHandle hiz_pyramid, const SceneView& view, size_t instance_count) {
		if (!pipeline) {
			std::string err = "HiZCullingPass::add_to_graph called with null pipeline";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return {};
#endif
		}

		return render_graph.add_pass("Hi-Z Culling Pass",
			[=](RGBuilder& builder) {
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.read(hiz_pyramid, ResourceState::UnorderedAccess); // Keep in GENERAL so we can sample it in GENERAL layout
				RGHandle new_draw = builder.write(indirect_draw_buffer, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return new_draw;
			},
			[=, &render_graph, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				// Defensive: resource lookups may throw in Debug if handles are invalid.
				bud::graphics::BufferHandle inst_buf{};
				bud::graphics::BufferHandle ind_buf{};
				bud::graphics::BufferHandle stat_buf{};
				Texture* depth_tex = nullptr;
				try {
					inst_buf = render_graph.get_buffer(instance_buffer);
					ind_buf = render_graph.get_buffer(indirect_draw_buffer);
					stat_buf = render_graph.get_buffer(stats_buffer);
					depth_tex = render_graph.get_texture(hiz_pyramid);
				}
				catch (const std::exception& e) {
					bud::eprint("[HiZCullingPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!inst_buf.is_valid() || !ind_buf.is_valid() || !stat_buf.is_valid() || !depth_tex) {
					static bool printed = false;
					if (!printed) {
						bud::print("[HiZCullingPass] Warning: Missing resources! inst={} ind={} stat={} depth={}",
							inst_buf.is_valid(), ind_buf.is_valid(), stat_buf.is_valid(), (bool)depth_tex);
						printed = true;
					}
					std::string err = std::format("HiZCullingPass missing resources: inst={} ind={} stat={} depth={}", inst_buf.is_valid(), ind_buf.is_valid(), stat_buf.is_valid(), (bool)depth_tex);
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				// Clear stats buffer (all counters = 0)
				bud::graphics::GPUStats zero_stats{};
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, stat_buf, 0, 24, &zero_stats);

				// Barrier: ensure the UpdateBuffer write is visible to the compute shader
				rhi->resource_barrier(cmd, stat_buf, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);

				rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, ind_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 2, stat_buf);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 3, depth_tex, ALL_MIPS, false, true); // is_general=true: pyramid was written as storage image
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 4);

				struct PushConsts {
					uint32_t instanceCount;
				} pc;
				pc.instanceCount = static_cast<uint32_t>(instance_count);

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

				// Dispatch 1 thread per instance
				uint32_t group_x = (static_cast<uint32_t>(instance_count) + 255) / 256;
				rhi->cmd_dispatch(cmd, group_x, 1, 1);
			}
		);
	}

	void MeshletFrustumCullingPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("MeshletFrustumCullingPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/meshlet_frustum_cull.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::MeshletFrustum;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) {
				bud::print("[MeshletFrustumCullingPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle MeshletFrustumCullingPass::add_to_graph(RenderGraph& render_graph, RGHandle instance_buffer, RGHandle meshlet_visibility_buffer, RGHandle stats_buffer, const SceneView& view, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes, const std::vector<SortItem>& sort_list, size_t visible_count, const GPUScene& gpu_scene) {
		if (!pipeline) {
			bud::eprint("[MeshletFrustumCullingPass] Skipping pass because pipeline is not ready yet.");
			return {};
		}

		return render_graph.add_pass("Meshlet Frustum Culling Pass",
			[=](RGBuilder& builder) {
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.write(meshlet_visibility_buffer, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return RGHandle{};
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				bud::graphics::BufferHandle inst_buf{};
				bud::graphics::BufferHandle vis_buf{};
				bud::graphics::BufferHandle stat_buf{};
				try {
					inst_buf = render_graph.get_buffer(instance_buffer);
					vis_buf = render_graph.get_buffer(meshlet_visibility_buffer);
					stat_buf = render_graph.get_buffer(stats_buffer);
				}
				catch (const std::exception& e) {
					bud::eprint("[MeshletFrustumCullingPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!inst_buf.is_valid() || !vis_buf.is_valid() || !stat_buf.is_valid()) {
					std::string err = std::format("MeshletFrustumCullingPass missing resources: inst={} vis={} stat={}", inst_buf.is_valid(), vis_buf.is_valid(), stat_buf.is_valid());
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				bud::graphics::GPUStats zero_stats{};
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, stat_buf, 0, 24, &zero_stats);
				rhi->resource_barrier(cmd, stat_buf, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, inst_buf);

				// Bind inst_buf to unused bindings to satisfy Vulkan Validation Layers
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 2, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 3, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 4, inst_buf);

				rhi->cmd_bind_storage_buffer(cmd, pipeline, 5, vis_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 6, stat_buf);
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 7);

				auto pt_buf = gpu_scene.get_page_table_buffer();
				auto pp_buf = gpu_scene.get_page_pool_buffer();
				if (pt_buf.is_valid()) rhi->cmd_bind_storage_buffer(cmd, pipeline, 10, pt_buf);
				if (pp_buf.is_valid()) rhi->cmd_bind_storage_buffer(cmd, pipeline, 11, pp_buf);

				const size_t dispatch_count = std::min(visible_count, sort_list.size());
				if (dispatch_count > 0) {
					struct PushConsts {
						uint32_t draw_count;
					} pc;
					pc.draw_count = static_cast<uint32_t>(dispatch_count);
					rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

					uint32_t group_x = (static_cast<uint32_t>(dispatch_count) + 255u) / 256u;
					rhi->cmd_dispatch(cmd, group_x, 1, 1);
				}
			}
		);
	}

	void HeuristicOccluderSelectionPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("HeuristicOccluderSelectionPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, {
			"src/shaders/heuristic_histogram.comp.spv",
			"src/shaders/heuristic_prefix_sum.comp.spv",
			"src/shaders/heuristic_occluder_select.comp.spv"
			}, [this, rhi](const auto& shaders) {
				ComputePipelineDesc desc;
				desc.layout_kind = ComputePipelineDesc::LayoutKind::HeuristicOccluder;

				desc.cs.code = shaders[0];
				histogram_pipeline = rhi->create_compute_pipeline(desc);

				desc.cs.code = shaders[1];
				prefix_sum_pipeline = rhi->create_compute_pipeline(desc);

				desc.cs.code = shaders[2];
				pipeline = rhi->create_compute_pipeline(desc);

				if (histogram_pipeline && prefix_sum_pipeline && pipeline) {
					bud::print("[HeuristicOccluderSelectionPass] Shaders loaded and 3 pipelines created.");
				}
			});

		config_ubo = rhi->create_gpu_buffer(sizeof(float) * 4, bud::graphics::ResourceState::UnorderedAccess);
		histogram_buffer = rhi->create_gpu_buffer(sizeof(uint32_t) * 2048, bud::graphics::ResourceState::UnorderedAccess);
	}

	void HeuristicOccluderSelectionPass::shutdown(RHI* rhi) {
		if (histogram_pipeline) { rhi->destroy_pipeline(histogram_pipeline); histogram_pipeline = nullptr; }
		if (prefix_sum_pipeline) { rhi->destroy_pipeline(prefix_sum_pipeline); prefix_sum_pipeline = nullptr; }
		if (pipeline) { rhi->destroy_pipeline(pipeline); pipeline = nullptr; }

		if (config_ubo.is_valid()) { rhi->destroy_buffer(config_ubo); config_ubo = {}; }
		if (histogram_buffer.is_valid()) { rhi->destroy_buffer(histogram_buffer); histogram_buffer = {}; }
	}

	struct HeuristicConfigLayout {
		float fraction;
		uint32_t cutoff_bucket;
		uint32_t remaining;
		uint32_t counter;
	};

	RGHandle HeuristicOccluderSelectionPass::add_to_graph(RenderGraph& render_graph, RGHandle instance_buffer, RGHandle meshlet_visibility_buffer, RGHandle indirect_draw_buffer, RGHandle stats_buffer, const SceneView& view, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes, const std::vector<SortItem>& sort_list, size_t visible_count, float occluder_fraction) {
		if (!pipeline) {
			bud::eprint("[HeuristicOccluderSelectionPass] Skipping pass because pipeline is not ready yet.");
			return {};
		}

		return render_graph.add_pass("Heuristic Occluder Selection Pass",
			[=](RGBuilder& builder) {
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.read(meshlet_visibility_buffer, ResourceState::ShaderResource);
				RGHandle new_draw = builder.write(indirect_draw_buffer, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return new_draw;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				bud::graphics::BufferHandle inst_buf{};
				bud::graphics::BufferHandle vis_buf{};
				bud::graphics::BufferHandle ind_buf{};
				bud::graphics::BufferHandle stat_buf{};
				try {
					inst_buf = render_graph.get_buffer(instance_buffer);
					vis_buf = render_graph.get_buffer(meshlet_visibility_buffer);
					ind_buf = render_graph.get_buffer(indirect_draw_buffer);
					stat_buf = render_graph.get_buffer(stats_buffer);
				}
				catch (const std::exception& e) {
					bud::eprint("[HeuristicOccluderSelectionPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!inst_buf.is_valid() || !vis_buf.is_valid() || !ind_buf.is_valid() || !stat_buf.is_valid()) {
					std::string err = std::format("HeuristicOccluderSelectionPass missing resources: inst={} vis={} ind={} stat={}", inst_buf.is_valid(), vis_buf.is_valid(), ind_buf.is_valid(), stat_buf.is_valid());
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				// --- Async Debug Logging (Previous Frame Results) ---
				frame_counter++;
				//if (frame_counter % 240 == 0) {
				//	// 1. Histogram Summary
				//	uint32_t* host_histogram = static_cast<uint32_t*>(histogram_buffer.mapped_ptr);
				//	uint32_t histogram_sum = 0;
				//	uint32_t max_bucket_val = 0;
				//	uint32_t max_bucket_idx = 0;
				//	if (host_histogram) {
				//		for (int i = 0; i < 2048; ++i) {
				//			histogram_sum += host_histogram[i];
				//			if (host_histogram[i] > max_bucket_val) {
				//				max_bucket_val = host_histogram[i];
				//				max_bucket_idx = i;
				//			}
				//		}
				//	}

				//	// 2. Cutoff results from config_ubo
				//	HeuristicConfigLayout* host_config = static_cast<HeuristicConfigLayout*>(config_ubo.mapped_ptr);

				//	// 4. Stats Buffer
				//	bud::graphics::GPUStats* host_stats = static_cast<bud::graphics::GPUStats*>(stat_buf.mapped_ptr);

				//	// 5. Indirect Buffer Emit
				//	IndirectCommand* host_ind = static_cast<IndirectCommand*>(ind_buf.mapped_ptr);
				//	uint32_t total_emitted_instances = 0;
				//	if (host_ind && host_stats) {
				//		// Note: host_stats->visibleInstances is the number of indirect commands emitted
				//		for (uint32_t i = 0; i < host_stats->visibleInstances; ++i) {
				//			total_emitted_instances += host_ind[i].instance_count;
				//		}
				//	}
				//	else if (!host_ind) {
				//		bud::eprint("[HeuStats][Warning] IndirectDrawBuffer is NOT host-mapped! Cannot verify emitted count.");
				//	}

				//	bud::print("[HeuStats][Histogram] total={} buckets=2048 dispatched={}", histogram_sum, static_cast<uint32_t>(visible_count));
				//	if (host_config) {
				//		bud::print("[HeuStats][Cutoff] frac={:.3f} bucket={} remaining={} counter={}", host_config->fraction, host_config->cutoff_bucket, host_config->remaining, host_config->counter);
				//		bud::print("[HeuStats][CutoffCounter] final={} expected_remaining={}", host_config->counter, host_config->remaining);
				//	}
				//	if (host_stats) {
				//		bud::print("[HeuStats][Stats] total={} vis={} tri_total={} tri_vis={} meshlet_total={} meshlet_vis={}", host_stats->totalInstances, host_stats->visibleInstances, host_stats->totalTriangles, host_stats->visibleTriangles, host_stats->totalMeshlets, host_stats->visibleMeshlets);
				//		bud::print("[HeuStats][Heuristics] heu_total={} heu_cutoff_bucket={} heu_remaining={}", host_stats->heuristicTotalCount, host_stats->heuristicCutoffBucket, host_stats->heuristicRemaining);
				//	}
				//	bud::print("[HeuStats][Indirect] emitted={} expected_vis={}", total_emitted_instances, (host_stats ? host_stats->visibleInstances : 0));
				//	if (host_ind && host_stats && host_stats->visibleInstances > 0) {
				//		bud::print("[HeuStats][Sample] sample0=(inst={} first_inst={})", host_ind[0].instance_count, host_ind[0].first_instance);
				//	}
				//	if (host_stats && host_stats->visibleInstances > 0 && total_emitted_instances == 0) {
				//		bud::eprint("[HeuStats][Error] Discrepancy: Stats show {} visible instances, but IndirectBuffer emitted 0!", host_stats->visibleInstances);
				//	}
				//	bud::print("[HeuStats][Hotspot] max_count={} bucket={}", max_bucket_val, max_bucket_idx);
				//}

				bud::graphics::GPUStats zero_stats{};
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, stat_buf, 0, sizeof(bud::graphics::GPUStats), &zero_stats);
				rhi->resource_barrier(cmd, stat_buf, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				// Upload occluder fraction to per-pass storage buffer and bind at binding = 5
				rhi->resource_barrier(cmd, config_ubo, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, config_ubo, 0, sizeof(float), &occluder_fraction);
				rhi->resource_barrier(cmd, config_ubo, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				// Zero out Histogram and Cutoff part of config_ubo
				uint32_t zero_histogram[2048] = { 0 };
				uint32_t zero_cutoff_data[3] = { 0 }; // cutoff_bucket, remaining, counter

				rhi->resource_barrier(cmd, histogram_buffer, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, histogram_buffer, 0, sizeof(uint32_t) * 2048, zero_histogram);
				rhi->resource_barrier(cmd, histogram_buffer, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				rhi->resource_barrier(cmd, config_ubo, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, config_ubo, sizeof(float), sizeof(uint32_t) * 3, zero_cutoff_data);
				rhi->resource_barrier(cmd, config_ubo, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				uint32_t dispatch_instances = static_cast<uint32_t>(visible_count);
				uint32_t push_constant = dispatch_instances;

				auto start_time = std::chrono::high_resolution_clock::now();

				// --- Pass 0: Histogram ---
				rhi->cmd_bind_pipeline(cmd, histogram_pipeline);
				rhi->cmd_push_constants(cmd, histogram_pipeline, sizeof(uint32_t), &push_constant);
				rhi->cmd_bind_storage_buffer(cmd, histogram_pipeline, 0, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, histogram_pipeline, 1, vis_buf);
				rhi->cmd_bind_storage_buffer(cmd, histogram_pipeline, 2, histogram_buffer);
				rhi->cmd_bind_storage_buffer(cmd, histogram_pipeline, 3, stat_buf);
				rhi->cmd_bind_compute_ubo(cmd, histogram_pipeline, 4);
				uint32_t group_x = (dispatch_instances + 255) / 256;
				rhi->cmd_dispatch(cmd, group_x, 1, 1);

				// Memory Barrier before Pass 1 so histogram is written
				rhi->resource_barrier(cmd, histogram_buffer, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);

				auto t1 = std::chrono::high_resolution_clock::now();

				// --- Pass 1: Prefix Sum ---
				rhi->cmd_bind_pipeline(cmd, prefix_sum_pipeline);
				rhi->cmd_bind_storage_buffer(cmd, prefix_sum_pipeline, 0, histogram_buffer);
				rhi->cmd_bind_storage_buffer(cmd, prefix_sum_pipeline, 3, stat_buf); // Pass 1 sets summary GPU stats
				rhi->cmd_bind_compute_ubo(cmd, prefix_sum_pipeline, 4); // Added missing UBO binding for complete descriptor set
				rhi->cmd_bind_storage_buffer(cmd, prefix_sum_pipeline, 5, config_ubo);
				rhi->cmd_dispatch(cmd, 1, 1, 1);

				// Memory Barrier before Pass 2 so cutoff data and stats summary are written
				rhi->resource_barrier(cmd, config_ubo, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);

				auto t2 = std::chrono::high_resolution_clock::now();

				// --- Pass 2: Selection & Emit ---
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_push_constants(cmd, pipeline, sizeof(uint32_t), &push_constant);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, vis_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 2, ind_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 3, stat_buf);
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 4);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 5, config_ubo);

				rhi->cmd_dispatch(cmd, group_x, 1, 1);

				auto end_time = std::chrono::high_resolution_clock::now();

				//if (frame_counter % 240 == 0) {
				//	std::chrono::duration<float, std::milli> p0 = t1 - start_time;
				//	std::chrono::duration<float, std::milli> p1 = t2 - t1;
				//	std::chrono::duration<float, std::milli> p2 = end_time - t2;
				//	bud::print("[HeuStats][Timing] pass0={:.3f}ms pass1={:.3f}ms pass2={:.3f}ms total={:.3f}ms", p0.count(), p1.count(), p2.count(), p0.count() + p1.count() + p2.count());
				//}
			}
		);
	}

	void MeshletHiZCullingPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("MeshletHiZCullingPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/meshlet_hiz_cull.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::MeshletHiZ;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) {
				bud::print("[MeshletHiZCullingPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle MeshletHiZCullingPass::add_to_graph(RenderGraph& render_graph, RGHandle instance_buffer, RGHandle meshlet_visibility_in, RGHandle meshlet_visibility_out, RGHandle stats_buffer, RGHandle hiz_pyramid, const SceneView& view, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes, const std::vector<SortItem>& sort_list, size_t visible_count, const GPUScene& gpu_scene) {
		if (!pipeline) {
			bud::eprint("[MeshletHiZCullingPass] Skipping pass because pipeline is not ready yet.");
			return {};
		}

		return render_graph.add_pass("Meshlet HiZ Culling Pass",
			[=](RGBuilder& builder) {
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.read(meshlet_visibility_in, ResourceState::ShaderResource);
				builder.read(hiz_pyramid, ResourceState::UnorderedAccess);
				builder.write(meshlet_visibility_out, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return RGHandle{};
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				bud::graphics::BufferHandle inst_buf{};
				bud::graphics::BufferHandle vis_in_buf{};
				bud::graphics::BufferHandle vis_out_buf{};
				bud::graphics::BufferHandle stat_buf{};
				Texture* hiz_tex = nullptr;
				try {
					inst_buf = render_graph.get_buffer(instance_buffer);
					vis_in_buf = render_graph.get_buffer(meshlet_visibility_in);
					vis_out_buf = render_graph.get_buffer(meshlet_visibility_out);
					stat_buf = render_graph.get_buffer(stats_buffer);
					hiz_tex = render_graph.get_texture(hiz_pyramid);
				}
				catch (const std::exception& e) {
					bud::eprint("[MeshletHiZCullingPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!inst_buf.is_valid() || !vis_in_buf.is_valid() || !vis_out_buf.is_valid() || !stat_buf.is_valid() || !hiz_tex) {
					std::string err = std::format("MeshletHiZCullingPass missing resources: inst={} vis_in={} vis_out={} stat={} hiz={}", inst_buf.is_valid(), vis_in_buf.is_valid(), vis_out_buf.is_valid(), stat_buf.is_valid(), (bool)hiz_tex);
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				bud::graphics::GPUStats zero_stats{};
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, stat_buf, 0, 24, &zero_stats);
				rhi->resource_barrier(cmd, stat_buf, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, inst_buf);
				// Bindings 1-4 (meshlet_data / vertex_index / meshlet_index /
				// meshlet_cull_data) are only read for non-page mesh triangle
				// stats. Bind valid buffers so the descriptors are initialized
				// (the page pool path supplies real data for page-backed meshes).
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 2, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 3, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 4, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 6, vis_in_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 7, vis_out_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 8, stat_buf);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 5, hiz_tex, ALL_MIPS, false, true);
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 9);

				auto pt_buf = gpu_scene.get_page_table_buffer();
				auto pp_buf = gpu_scene.get_page_pool_buffer();
				if (pt_buf.is_valid())
					rhi->cmd_bind_storage_buffer(cmd, pipeline, 10, pt_buf);
				if (pp_buf.is_valid())
					rhi->cmd_bind_storage_buffer(cmd, pipeline, 11, pp_buf);

				const size_t dispatch_count = std::min(visible_count, sort_list.size());
				if (dispatch_count > 0) {
					struct PushConsts {
						uint32_t drawCount;
					} pc;
					pc.drawCount = static_cast<uint32_t>(dispatch_count);
					rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

					uint32_t group_x = (static_cast<uint32_t>(dispatch_count) + 255u) / 256u;
					rhi->cmd_dispatch(cmd, group_x, 1, 1);
				}
			}
		);
	}

	void MeshletIndirectEmissionPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("MeshletIndirectEmissionPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/meshlet_indirect_emit.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::MeshletIndirect;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) {
				bud::print("[MeshletIndirectEmissionPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle MeshletIndirectEmissionPass::add_to_graph(RenderGraph& render_graph, RGHandle instance_buffer, RGHandle meshlet_visibility_buffer, RGHandle indirect_draw_buffer, RGHandle stats_buffer, const SceneView& view, const RenderScene& render_scene, const std::vector<RenderMesh>& meshes, const std::vector<SortItem>& sort_list, size_t visible_count, const GPUScene& gpu_scene) {
		if (!pipeline) {
			bud::eprint("[MeshletIndirectEmissionPass] Skipping pass because pipeline is not ready yet.");
			return {};
		}

		return render_graph.add_pass("Meshlet Indirect Emission Pass",
			[=](RGBuilder& builder) {
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.read(meshlet_visibility_buffer, ResourceState::ShaderResource);
				RGHandle new_draw = builder.write(indirect_draw_buffer, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return new_draw;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				bud::graphics::BufferHandle inst_buf{};
				bud::graphics::BufferHandle vis_buf{};
				bud::graphics::BufferHandle draw_buf{};
				bud::graphics::BufferHandle stat_buf{};
				try {
					inst_buf = render_graph.get_buffer(instance_buffer);
					vis_buf = render_graph.get_buffer(meshlet_visibility_buffer);
					draw_buf = render_graph.get_buffer(indirect_draw_buffer);
					stat_buf = render_graph.get_buffer(stats_buffer);
				}
				catch (const std::exception& e) {
					bud::eprint("[MeshletIndirectEmissionPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!inst_buf.is_valid() || !vis_buf.is_valid() || !draw_buf.is_valid() || !stat_buf.is_valid()) {
					std::string err = std::format("MeshletIndirectEmissionPass missing resources: inst={} vis={} draw={} stat={}", inst_buf.is_valid(), vis_buf.is_valid(), draw_buf.is_valid(), stat_buf.is_valid());
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				bud::graphics::GPUStats zero_stats{};
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, stat_buf, 0, 24, &zero_stats);
				rhi->resource_barrier(cmd, stat_buf, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, vis_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 2, draw_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 3, stat_buf);
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 4);

				auto pt_buf = gpu_scene.get_page_table_buffer();
				auto pp_buf = gpu_scene.get_page_pool_buffer();
				if (pt_buf.is_valid())
					rhi->cmd_bind_storage_buffer(cmd, pipeline, 5, pt_buf);
				if (pp_buf.is_valid())
					rhi->cmd_bind_storage_buffer(cmd, pipeline, 6, pp_buf);

				const size_t dispatch_count = std::min(visible_count, sort_list.size());
				if (dispatch_count > 0) {
					struct PushConsts {
						uint32_t drawCount;
					} pc;
					pc.drawCount = static_cast<uint32_t>(dispatch_count);
					rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

					uint32_t group_x = (static_cast<uint32_t>(dispatch_count) + 255u) / 256u;
					rhi->cmd_dispatch(cmd, group_x, 1, 1);
				}
			}
		);
	}

	RGHandle DepthOnlyPass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		size_t instance_count,
		RGHandle indirect_draw_buffer,
		const GPUScene& gpu_scene,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer,
        RGHandle existing_depth_buffer,
		size_t split_index) {
		if (!pipeline) {
			std::string err = "DepthOnlyPass::add_to_graph pipeline is null";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return {};
#endif
		}

		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
			});

		const bool use_indirect_draw = indirect_draw_buffer.is_valid();
		if (max_scene_count == 0 || (!use_indirect_draw && sort_list.empty())) {
			std::string err = "ZPrepass::add_to_graph empty scene or sort list";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return {};
#endif
		}

		Texture* backbuffer_tex = nullptr;
		try {
			backbuffer_tex = render_graph.get_texture(backbuffer);
		}
		catch (const std::exception& e) {
			bud::eprint("ZPrepass::add_to_graph: failed to get backbuffer texture: {}", e.what());
#if defined(_DEBUG)
			throw;
#else
			return {};
#endif
		}
		if (!backbuffer_tex || backbuffer_tex->width == 0 || backbuffer_tex->height == 0) {
			bud::eprint("ZPrepass::add_to_graph invalid backbuffer: tex={} w={} h={}", (void*)backbuffer_tex, backbuffer_tex ? backbuffer_tex->width : 0, backbuffer_tex ? backbuffer_tex->height : 0);
#if defined(_DEBUG)
			throw std::runtime_error("ZPrepass::add_to_graph invalid backbuffer");
#else
			return {};
#endif
		}

		// instance_count = exploded submesh draw count; sort_list is sized to match.
		// max_scene_count guards accessing render_scene arrays, but entity_index in
		// sort_list items are already validated — do NOT clamp draw_count by it.
		const size_t draw_count = std::min(instance_count, sort_list.size());
		uint32_t target_width = backbuffer_tex->width;
		uint32_t target_height = backbuffer_tex->height;

		TextureDesc depth_desc;
		depth_desc.width = target_width;
		depth_desc.height = target_height;
		depth_desc.format = bud::graphics::TextureFormat::D32_FLOAT;

		auto depth_h = std::make_shared<RGHandle>(existing_depth_buffer);
		bool clear_depth_flag = !existing_depth_buffer.is_valid();
        
		return render_graph.add_pass(clear_depth_flag ? "Depth Only Pass" : "Depth Only Pass (Phase 2)",
			[=](RGBuilder& builder) {
				if (clear_depth_flag) {
					*depth_h = builder.create("MainDepth", depth_desc);
				}
				*depth_h = builder.write(*depth_h, ResourceState::DepthWrite);
				if (use_indirect_draw && indirect_draw_buffer.is_valid()) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				return *depth_h;
			},
			// Capture `sort_list` by reference (owned by caller) - caller must ensure lifetime
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) {
					bud::eprint("[DepthOnlyPass] ERROR: Pipeline is null.");
					return;
				}


				RenderPassBeginInfo info;
				info.depth_attachment = render_graph.get_texture(*depth_h);
				info.clear_depth = clear_depth_flag;
				info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				const auto pp_buf = gpu_scene.get_page_pool_buffer();

				if (use_indirect_draw) {
					bud::graphics::BufferHandle ind_buf_handle;
					try {
						ind_buf_handle = render_graph.get_buffer(indirect_draw_buffer);
					}
					catch (const std::exception& e) {
						bud::eprint("[DepthOnlyPass] failed to get indirect draw buffer: {}", e.what());
						return;
					}

					if (!ind_buf_handle.is_valid()) {
						bud::eprint("[DepthOnlyPass] invalid indirect draw buffer.");
						return;
					}

					if (config.enable_gpu_driven) {
						if (indirect_draw_buffer.is_valid() && draw_count > 0) {
							auto pp_buf = gpu_scene.get_page_pool_buffer();

							if (split_index > 0) {
								rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
								rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
								rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), 0, static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
							}

							if (split_index < draw_count && pp_buf.is_valid()) {
								rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
								rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
								rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), split_index * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(draw_count - split_index), sizeof(bud::graphics::IndirectCommand));
							}
						}
					}
				}
				else {
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						uint32_t material_id = render_scene.material_indices[idx];
						const auto& model_matrix = render_scene.world_matrices[idx];

						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						// Page-backed meshes are backed by the GPU page pool, not the mega
						// geometry pool, so rebind the buffers before issuing the draw.
						if (mesh.is_page_backed && gpu_scene.get_page_pool_buffer().is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, gpu_scene.get_page_pool_buffer());
							rhi->cmd_bind_index_buffer(cmd, gpu_scene.get_page_pool_buffer(), true);
						}
						else {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						}

						uint32_t vtx_off = (mesh.is_page_backed && gpu_scene.get_page_pool_buffer().is_valid()) ? 0 : mesh_geometry.vertex_offset;
						if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[item.submesh_index];
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, vtx_off, (uint32_t)i);
						}
						else {
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, vtx_off, (uint32_t)i);
						}
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}



	void HiZMipPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("HiZMipPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/hiz_mip.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::HiZMip;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) {
				bud::print("[HiZMipPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle HiZMipPass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const RenderConfig& config) {
		if (!pipeline) return {};

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0) return {};

		// Create a POT pyramid texture for easy mip generation
		uint32_t pot_w = 1 << (uint32_t)std::ceil(std::log2((float)depth_desc.width));
		uint32_t pot_h = 1 << (uint32_t)std::ceil(std::log2((float)depth_desc.height));
		uint32_t size = std::max(pot_w, pot_h);
		uint32_t mip_count = (uint32_t)std::floor(std::log2((float)size)) + 1;

		TextureDesc desc;
		desc.width = size;
		desc.height = size;
		desc.mips = mip_count;
		desc.format = bud::graphics::TextureFormat::R32_FLOAT; // Standard format for HiZ
		desc.is_storage = true;
		desc.initial_state = bud::graphics::ResourceState::Undefined;

		auto pyramid_h_ptr = std::make_shared<RGHandle>();

		for (uint32_t i = 0; i < mip_count; ++i) {
			rg.add_pass(std::format("Hi-Z Mip {}", i),
				[=](RGBuilder& builder) {
					if (i == 0) {
						*pyramid_h_ptr = builder.create("HiZPyramid", desc);
					}
					RGHandle current_pyramid = *pyramid_h_ptr;
					RGHandle src_handle = (i == 0) ? depth_buffer : current_pyramid;
					ResourceState src_read_state = (i == 0) ? ResourceState::ShaderResource : ResourceState::UnorderedAccess;

					builder.read(src_handle, src_read_state);
					builder.write(current_pyramid, ResourceState::UnorderedAccess);
					return current_pyramid;
				},
				[=, &rg](RHI* rhi, CommandHandle cmd) {
					RGHandle current_pyramid = *pyramid_h_ptr;
					RGHandle src_handle = (i == 0) ? depth_buffer : current_pyramid;

					uint32_t dst_size = size >> i;
					rhi->cmd_bind_pipeline(cmd, pipeline);
					// Defensive: fetch textures with try/catch
					Texture* src_tex = nullptr;
					Texture* dst_tex = nullptr;
					try {
						src_tex = rg.get_texture(src_handle);
						dst_tex = rg.get_texture(current_pyramid);
					}
					catch (const std::exception& e) {
						bud::eprint("[HiZMipPass] Resource lookup failed: {}", e.what());
						return;
					}
					rhi->cmd_bind_compute_texture(cmd, pipeline, 3, src_tex, (i == 0) ? 0 : (i - 1), false, (i > 0)); // is_general=true when reading from the pyramid (it's in GENERAL layout)
					rhi->cmd_bind_compute_texture(cmd, pipeline, 5, dst_tex, i, true);

					struct Push {
						bud::math::vec2 out_size;
						uint32_t reversed_z;
						uint32_t padding;
					} push;
					push.out_size = bud::math::vec2((float)dst_size, (float)dst_size);
					push.reversed_z = config.reversed_z ? 1 : 0;
					rhi->cmd_push_constants(cmd, pipeline, sizeof(push), &push);

					uint32_t gx = (dst_size + 15) / 16;
					uint32_t gy = (dst_size + 15) / 16;
					rhi->cmd_dispatch(cmd, gx, gy, 1);
				}
			);
		}

		return *pyramid_h_ptr;
	}

	void HiZDebugPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("HiZDebugPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/fullscreen.vert.spv", "src/shaders/hiz_debug.frag.spv" }, [this, rhi](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = false;
			desc.depth_write = false;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.vertex_layout = VertexLayoutType::NoVertexInput;
			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline) {
				bud::print("[HiZDebugPass] Shaders loaded and pipeline created.");
			}
			});
	}

	void HiZDebugPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle hiz_pyramid, uint32_t mip_level) {
		if (!pipeline) return;
		rg.add_pass("Hi-Z Debug Pass",
			[=](RGBuilder& builder) {
				builder.read(hiz_pyramid, ResourceState::ShaderResource);
				builder.write(backbuffer, ResourceState::RenderTarget);
			},
			[=, &rg](RHI* rhi, CommandHandle cmd) {
				rhi->cmd_begin_debug_label(cmd, "Hi-Z Debug", 1, 0, 1);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->update_bindless_image(0, rg.get_texture(hiz_pyramid), ALL_MIPS, false);

				struct PC { uint32_t mip; } pc = { mip_level };
				rhi->cmd_push_constants(cmd, pipeline, sizeof(pc), &pc);

				RenderPassBeginInfo info;
				info.color_attachments.push_back(rg.get_texture(backbuffer));
				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_draw(cmd, 3, 1, 0, 0);
				rhi->cmd_end_render_pass(cmd);
				rhi->cmd_end_debug_label(cmd);
			}
		);
	}

	void DepthOnlyPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("DepthOnlyPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/depth_only.vert.spv", "src/shaders/depth_only.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::Undefined;
			desc.depth_compare_op = config.reversed_z ? CompareOp::Greater : CompareOp::Less;
			desc.enable_depth_bias = false;
			desc.vertex_layout = VertexLayoutType::PositionUV;

			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline) {
				bud::print("[DepthOnlyPass] Shaders loaded and pipeline created.");
			}
			});
	}

	CSMShadowPass::~CSMShadowPass() {
	}

	void CSMShadowPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (rhi) {
			auto* pool = rhi->get_resource_pool();
			if (pool && static_cache_texture) {
				pool->release_texture(static_cache_texture);
			}
		}

		static_cache_texture = nullptr;
		cache_initialized = false;
		has_last_view_proj = false;
		has_last_config = false;
	}

	void CSMShadowPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("CSMShadowPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		stored_rhi = rhi;
		load_shaders_async(asset_manager, { "src/shaders/shadow.vert.spv", "src/shaders/shadow.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.cull_mode = CullMode::Back;
			desc.color_attachment_format = TextureFormat::Undefined;
			desc.depth_compare_op = config.reversed_z ? CompareOp::Greater : CompareOp::Less;
			desc.enable_depth_bias = true;
			desc.vertex_layout = VertexLayoutType::PositionUV;

			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline) {
				bud::print("[CSMShadowPass] Shaders loaded and pipeline created: {}", (void*)pipeline);
			}
			});

		load_shaders_async(asset_manager, { "src/shaders/csm_cull.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.cs.code = shaders[0];
			desc.layout_kind = ComputePipelineDesc::LayoutKind::HeuristicOccluder; // Shares same bindings! (InstanceData, IndirectDraw, ubo, pc)

			csm_cull_pipeline = rhi->create_compute_pipeline(desc);
			if (csm_cull_pipeline) {
				bud::print("[CSMShadowPass] csm_cull pipeline created: {}", (void*)csm_cull_pipeline);
			}
			});
	}

	void MainPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/main.vert.spv", "src/shaders/main.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
			desc.enable_depth_bias = false;
			desc.vertex_layout = VertexLayoutType::Default;

			pipeline = rhi->create_graphics_pipeline(desc);

			desc.wireframe = true;
			pipeline_wireframe = rhi->create_graphics_pipeline(desc);

			if (pipeline && pipeline_wireframe) {
				bud::print("[MainPass] Shaders loaded and pipelines created.");
			}
			});
	}

	void MainPass::shutdown(RHI* rhi) {
		if (pipeline) rhi->destroy_pipeline(pipeline);
		if (pipeline_wireframe) rhi->destroy_pipeline(pipeline_wireframe);
		pipeline = nullptr;
		pipeline_wireframe = nullptr;
	}

	void MainPass::add_to_graph(RenderGraph& render_graph, RGHandle shadow_map, RGHandle backbuffer, RGHandle depth_buffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		size_t instance_count,
		RGHandle indirect_draw_buffer,
		RGHandle instance_data,
		const GPUScene& gpu_scene,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer,
		RGHandle ao_map,
		size_t split_index)
	{
		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
			});

		if (max_scene_count == 0 || sort_list.empty()) {
			bud::eprint("[MainPass] ERROR: RenderScene or sort list is empty.");
			return;
		}

		const auto* backbuffer_tex = render_graph.get_texture(backbuffer);
		if (!backbuffer_tex || backbuffer_tex->width == 0 || backbuffer_tex->height == 0) {
			return;
		}

		const size_t draw_count = std::min(instance_count, sort_list.size());

		uint32_t target_width = backbuffer_tex->width;
		uint32_t target_height = backbuffer_tex->height;

		render_graph.add_pass("Main Lighting Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.read(shadow_map, ResourceState::DepthRead);
				builder.write(depth_buffer, ResourceState::DepthWrite);
				if (ao_map.is_valid()) {
					builder.read(ao_map, ResourceState::ShaderResource);
				}
				if (config.enable_gpu_driven) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				builder.read(instance_data, ResourceState::ShaderResource);
				return depth_buffer;
			},

			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline || !pipeline_wireframe) {
					bud::eprint("[MainPass] ERROR: Pipeline is null.");
					return;
				}

				void* active_pipeline = config.enable_wireframe ? pipeline_wireframe : pipeline;

				if (ao_map.is_valid()) {
					Texture* ao_tex = render_graph.get_texture(ao_map);
					if (ao_tex) rhi->update_bindless_texture_current_frame(ao_map_bindless_slot, ao_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(ao_map_bindless_slot, rhi->get_fallback_texture());
				}

				bud::graphics::BufferHandle ind_buf_handle;
				if (config.enable_gpu_driven) {
					ind_buf_handle = render_graph.get_buffer(indirect_draw_buffer);
				}

				RenderPassBeginInfo info;
				info.color_attachments.push_back(render_graph.get_texture(backbuffer));
				info.depth_attachment = render_graph.get_texture(depth_buffer);
				info.clear_color = true;
				info.clear_color_value = { 0.5f, 0.5f, 0.5f, 1.0f };
				info.clear_depth = false;
				info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				// Global Set Bindings
				rhi->update_global_shadow_map(render_graph.get_texture(shadow_map));
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				// Instance Data is already bound to Set 0, Binding 3 by Renderer calling rhi->update_global_instance_data
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0);

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
					if (config.enable_gpu_driven) {
						if (indirect_draw_buffer.is_valid() && draw_count > 0) {
							auto pp_buf = gpu_scene.get_page_pool_buffer();
							
							if (split_index > 0) {
								rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
								rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
								rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), 0, static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
							}

							if (split_index < draw_count && pp_buf.is_valid()) {
								rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
								rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
								rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), split_index * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(draw_count - split_index), sizeof(bud::graphics::IndirectCommand));
							}
						}
					}
				else {
					auto pp_buf = gpu_scene.get_page_pool_buffer();
					size_t page_backed_draws = 0;
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;

						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						if (mesh.is_page_backed && pp_buf.is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
							rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
							// Multi-material pages carry per-material submeshes; draw the
							// specific one when the sort item selects it.
							if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
								const auto& sub = mesh.submeshes[item.submesh_index];
								rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, 0, (uint32_t)i);
							}
							else {
								rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, 0, (uint32_t)i);
							}
							page_backed_draws++;
						}
						else if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							// Rebind mega buffers: the previous draw may have left the page pool bound.
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
							const auto& sub = mesh.submeshes[item.submesh_index];
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, (uint32_t)i);
						}
						else {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, mesh_geometry.vertex_offset, (uint32_t)i);
						}
					}
					if (page_backed_draws > 0) {
						const auto& first_mesh_geom = gpu_scene.get_mesh_geometry(0);
						//bud::print("[MainPass] page_backed_draws={} first_index={} vertex_offset={} pp_buf_valid={}",
						//	page_backed_draws, first_mesh_geom.first_index, first_mesh_geom.vertex_offset, pp_buf.is_valid());
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}

	void RenderPass::shutdown(RHI* rhi) {
		if (pipeline && rhi) {
			rhi->destroy_pipeline(pipeline);
			pipeline = nullptr;
		}
	}

	UIPass::~UIPass() {}

	void UIPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (font_texture && rhi) {
			auto* pool = rhi->get_resource_pool();
			if (pool) pool->release_texture(font_texture);
			font_texture = nullptr;
		}
		if (current_vertex_buffer_size > 0 && rhi) {
			rhi->destroy_buffer(std::move(vertex_buffer));
			current_vertex_buffer_size = 0;
		}
		if (current_index_buffer_size > 0 && rhi) {
			rhi->destroy_buffer(std::move(index_buffer));
			current_index_buffer_size = 0;
		}
	}

	void UIPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		// Build font texture FIRST
		ImGuiIO& imgui_io = ImGui::GetIO();
		unsigned char* pixels;
		int width, height;
		imgui_io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

		TextureDesc tex_desc;
		tex_desc.width = width;
		tex_desc.height = height;
		tex_desc.format = TextureFormat::RGBA8_SRGB;
		tex_desc.mips = 1;

		font_texture = rhi->create_texture(tex_desc, pixels, width * height * 4);
		rhi->set_debug_name(font_texture, ObjectType::Texture, "ImGui_Font_Atlas");

		font_bindless_index = imgui_font_bindless_slot;
		rhi->update_bindless_texture(font_bindless_index, font_texture);
		imgui_io.Fonts->SetTexID((ImTextureID)(intptr_t)font_bindless_index);

		load_shaders_async(asset_manager, { "src/shaders/debug_ui.vert.spv", "src/shaders/debug_ui.frag.spv" }, [this, rhi](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = false;
			desc.depth_write = false;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.depth_attachment_format = bud::graphics::TextureFormat::Undefined;
			desc.depth_compare_op = CompareOp::Always;
			desc.enable_depth_bias = false;
			desc.blending_enable = true;
			desc.vertex_layout = VertexLayoutType::ImGui;

			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline) {
				bud::print("[UIPass] Shaders loaded and pipeline created.");
			}
			});
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
						.texture_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>((void*)src_cmd.GetTexID()))
						});
				}

				ui_draw_data_snapshot.lists.push_back(std::move(dst_list));
			}
		}

		std::lock_guard lock(draw_data_mutex);
		cached_draw_data = std::move(ui_draw_data_snapshot);
	}

	void UIPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer) {
		rg.add_pass("UIPass",
			[&](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
			},
			[=, this](RHI* rhi, CommandHandle cmd) {
				UIDrawDataSnapshot draw_data;
				{
					std::lock_guard lock(draw_data_mutex);
					draw_data = cached_draw_data;
				}

				if (!draw_data.has_data())
					return;

				if (!pipeline)
					return;

				// Create or resize buffers. Use allocator->alloc_staging (per-frame ring) instead
				// of ad-hoc upload buffers. Use doubling growth strategy to avoid frequent reallocs.
				uint32_t needed_vb_size = draw_data.total_vtx_count() * sizeof(ImDrawVert);
				uint32_t needed_ib_size = draw_data.total_idx_count() * sizeof(uint32_t);

				// Vertex buffer growth (double strategy)
				if (needed_vb_size > current_vertex_buffer_size) {
					if (current_vertex_buffer_size > 0) {
						rhi->destroy_buffer(vertex_buffer);
					}
					uint32_t new_size = current_vertex_buffer_size ? std::max<uint32_t>(needed_vb_size, current_vertex_buffer_size * 2u) : std::max<uint32_t>(needed_vb_size, 4096u);
					current_vertex_buffer_size = new_size;
					auto* alloc = rhi->get_allocator();
					if (!alloc) {
						bud::eprint("[UIPass] No allocator available for staging vertex buffer");
						return;
					}
					vertex_buffer = alloc->alloc_staging(current_vertex_buffer_size);
					if (!vertex_buffer.is_valid() || !vertex_buffer.mapped_ptr) {
						bud::eprint("[UIPass] alloc_staging failed for vertex buffer size={}", current_vertex_buffer_size);
						return;
					}
				}

				// Index buffer growth (double strategy)
				if (needed_ib_size > current_index_buffer_size) {
					if (current_index_buffer_size > 0) {
						rhi->destroy_buffer(index_buffer);
					}
					uint32_t new_size = current_index_buffer_size ? std::max<uint32_t>(needed_ib_size, current_index_buffer_size * 2u) : std::max<uint32_t>(needed_ib_size, 4096u);
					current_index_buffer_size = new_size;
					auto* alloc = rhi->get_allocator();
					if (!alloc) {
						bud::eprint("[UIPass] No allocator available for staging index buffer");
						return;
					}
					index_buffer = alloc->alloc_staging(current_index_buffer_size);
					if (!index_buffer.is_valid() || !index_buffer.mapped_ptr) {
						bud::eprint("[UIPass] alloc_staging failed for index buffer size={}", current_index_buffer_size);
						return;
					}
				}

				auto* vtx_dst = (ImDrawVert*)vertex_buffer.mapped_ptr;
				auto* idx_dst = (uint32_t*)index_buffer.mapped_ptr;

				// Defensive checks
				if (!vtx_dst) {
					bud::eprint("[UIPass] vertex_buffer.mapped_ptr is null (size={})", current_vertex_buffer_size);
					return;
				}
				if (!idx_dst) {
					bud::eprint("[UIPass] index_buffer.mapped_ptr is null (size={})", current_index_buffer_size);
					return;
				}

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

				rhi->cmd_bind_vertex_buffer(cmd, vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, index_buffer);

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

	void ClusterVisualizationPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;
		load_shaders_async(asset_manager, { "src/shaders/cluster_debug.vert.spv", "src/shaders/cluster_debug.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
			desc.enable_depth_bias = false;
			desc.vertex_layout = VertexLayoutType::PositionNormal;

			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline) {
				bud::print("[ClusterVisualizationPass] Shaders loaded and pipeline created.");
			}
			});
	}

	void ClusterVisualizationPass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer, RGHandle depth_buffer,
		const RenderScene& render_scene, const SceneView& view, const RenderConfig& config,
		const std::vector<RenderMesh>& meshes, const std::vector<SortItem>& sort_list,
		size_t instance_count, bud::graphics::RGHandle indirect_draw_buffer,
		bud::graphics::RGHandle instance_data,
		const GPUScene& gpu_scene,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer,
		size_t split_index)
	{
		const size_t draw_count = std::min(instance_count, sort_list.size());
		if (draw_count == 0 || !pipeline) return;

		const auto* backbuffer_tex = render_graph.get_texture(backbuffer);
		if (!backbuffer_tex || backbuffer_tex->width == 0 || backbuffer_tex->height == 0) return;

		uint32_t target_width = backbuffer_tex->width;
		uint32_t target_height = backbuffer_tex->height;

		render_graph.add_pass("Cluster Visualization Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.write(depth_buffer, ResourceState::DepthWrite);
				if (config.enable_gpu_driven) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				builder.read(instance_data, ResourceState::ShaderResource);
				return backbuffer;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				bud::graphics::BufferHandle ind_buf_handle;
				if (config.enable_gpu_driven) ind_buf_handle = render_graph.get_buffer(indirect_draw_buffer);

				RenderPassBeginInfo info;
				info.color_attachments.push_back(render_graph.get_texture(backbuffer));
				info.depth_attachment = render_graph.get_texture(depth_buffer);
				info.clear_color = true;
				info.clear_color_value = { 0.1f, 0.1f, 0.1f, 1.0f };
				info.clear_depth = false; // depth comes from prepass

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				if (config.enable_gpu_driven && ind_buf_handle.is_valid()) {
					auto pp_buf = gpu_scene.get_page_pool_buffer();
					
					if (split_index > 0) {
						rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
						rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						rhi->cmd_draw_indexed_indirect(cmd, ind_buf_handle, 0, static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
					}

					if (split_index < draw_count && pp_buf.is_valid()) {
						rhi->cmd_bind_vertex_buffer(cmd, pp_buf);
						rhi->cmd_bind_index_buffer(cmd, pp_buf, true);
						rhi->cmd_draw_indexed_indirect(cmd, ind_buf_handle, split_index * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(draw_count - split_index), sizeof(bud::graphics::IndirectCommand));
					}
				}
				else {
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						if (mesh.is_page_backed && gpu_scene.get_page_pool_buffer().is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, gpu_scene.get_page_pool_buffer());
							rhi->cmd_bind_index_buffer(cmd, gpu_scene.get_page_pool_buffer(), true);
						}
						else {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						}

						if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[item.submesh_index];
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, (uint32_t)i);
						}
						else {
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, mesh_geometry.vertex_offset, (uint32_t)i);
						}
					}
				}
				rhi->cmd_end_render_pass(cmd);
			}
		);
	}

	void AmbientOcclusionPass::shutdown(RHI* rhi) {
		if (ssao_pipeline) { rhi->destroy_pipeline(ssao_pipeline); ssao_pipeline = nullptr; }
		if (gtao_pipeline) { rhi->destroy_pipeline(gtao_pipeline); gtao_pipeline = nullptr; }
	}

	void AmbientOcclusionPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/ssao.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AmbientOcclusion;
			desc.cs.code = shaders[0];
			ssao_pipeline = rhi->create_compute_pipeline(desc);
			if (ssao_pipeline) bud::print("[AmbientOcclusionPass] SSAO shader loaded and pipeline created.");
			});

		load_shaders_async(asset_manager, { "src/shaders/gtao.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AmbientOcclusion;
			desc.cs.code = shaders[0];
			gtao_pipeline = rhi->create_compute_pipeline(desc);
			if (gtao_pipeline) bud::print("[AmbientOcclusionPass] GTAO shader loaded and pipeline created.");
			});
	}

	RGHandle AmbientOcclusionPass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config) {
		if (config.ao_mode == AOMode::Disabled || !depth_buffer.is_valid()) return {};

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0) return {};

		// Evaluate AO at half resolution by default and let the AO blur pass
		// depth-aware-upsample it back to full resolution. The AO shaders map
		// UVs from screen_size, so they adapt to the smaller target.
		const uint32_t downscale = config.ao_half_res ? 2u : 1u;
		TextureDesc ao_desc{};
		ao_desc.width = std::max(1u, depth_desc.width / downscale);
		ao_desc.height = std::max(1u, depth_desc.height / downscale);
		// R32F: 8-bit R8_UNORM quantizes the temporally-smoothed AO into 256
		// levels, producing visible contour banding / moiré on smooth gradients.
		ao_desc.format = TextureFormat::R32_FLOAT;
		ao_desc.is_storage = true;

		auto raw_ao_h = std::make_shared<RGHandle>();

		return rg.add_pass("Ambient Occlusion Pass",
			[=](RGBuilder& builder) {
				*raw_ao_h = builder.create("RawAOTexture", ao_desc);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(*raw_ao_h, ResourceState::UnorderedAccess);
				return *raw_ao_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				void* active_pipeline = (config.ao_mode == AOMode::GTAO) ? gtao_pipeline : ssao_pipeline;
				if (!active_pipeline) return;

				Texture* depth_tex = nullptr;
				Texture* ao_tex = nullptr;
				try {
					depth_tex = rg.get_texture(depth_buffer);
					ao_tex = rg.get_texture(*raw_ao_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[AmbientOcclusionPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!depth_tex || !ao_tex) return;

				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_bind_compute_texture(cmd, active_pipeline, 0, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, active_pipeline, 1, ao_tex, 0, true);

				struct PushConsts {
					bud::math::mat4 proj;
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					float radius;
					float intensity;
					uint32_t sample_count;
					uint32_t reversed_z;
					float time;
				} pc;

				pc.proj = view.proj_matrix;
				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(ao_desc.width), static_cast<float>(ao_desc.height));
				pc.radius = config.ao_radius;
				pc.intensity = config.ao_intensity;
				pc.sample_count = config.ao_sample_count;
				pc.reversed_z = config.reversed_z ? 1 : 0;
				// Per-frame noise phase: wrap time to a 16s window to keep float32
				// precision, then scale by 37 so the phase advances ~0.6 per frame
				// (golden-ratio-like decorrelation for the temporal filter).
				// Per-frame noise phase: rotate the slice pattern ~17 deg/frame (23/60
				// at 60fps, coprime with 60 so it stays decorrelated over the ~19-frame
				// averaging window) -- large rotation steps (37) make consecutive
				// frames differ a lot, inflating the residual noise floor.
				pc.time = std::fmod(std::fmod(std::fabs(view.time), 16.0f) * 23.0f, 1.0f);

				rhi->cmd_push_constants(cmd, active_pipeline, sizeof(PushConsts), &pc);

				uint32_t gx = (ao_desc.width + 15u) / 16u;
				uint32_t gy = (ao_desc.height + 15u) / 16u;
				rhi->cmd_dispatch(cmd, gx, gy, 1);
			}
		);
	}

	void AOBlurPass::shutdown(RHI* rhi) {
		if (pipeline) { rhi->destroy_pipeline(pipeline); pipeline = nullptr; }
	}

	void AOBlurPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/ao_blur.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AOBlur;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) bud::print("[AOBlurPass] Shader loaded and pipeline created.");
			});
	}

	RGHandle AOBlurPass::add_to_graph(RenderGraph& rg, RGHandle raw_ao, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config) {
		if (!pipeline || !raw_ao.is_valid() || !depth_buffer.is_valid() || !config.ao_blur_enable) return raw_ao;

		auto raw_desc = rg.get_texture_desc(raw_ao);
		if (raw_desc.width == 0 || raw_desc.height == 0) return raw_ao;

		// The blur pass outputs at full resolution: it reads the lower-res raw
		// AO and depth-aware upsamples it (kernel steps are half-res texels).
		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0) return raw_ao;

		TextureDesc blur_desc{};
		blur_desc.width = depth_desc.width;
		blur_desc.height = depth_desc.height;
		blur_desc.format = raw_desc.format;
		blur_desc.is_storage = true;
		auto blurred_ao_h = std::make_shared<RGHandle>();

		return rg.add_pass("AO Blur Pass",
			[=](RGBuilder& builder) {
				*blurred_ao_h = builder.create("BlurredAOTexture", blur_desc);
				// Raw AO is read in GENERAL layout (is_general=true) so the history
				// texture feeding this pass is never taken out of GENERAL.
				builder.read(raw_ao, ResourceState::UnorderedAccess);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(*blurred_ao_h, ResourceState::UnorderedAccess);
				return *blurred_ao_h;
			},
			[=, &rg](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				Texture* raw_tex = nullptr;
				Texture* depth_tex = nullptr;
				Texture* blur_tex = nullptr;
				try {
					raw_tex = rg.get_texture(raw_ao);
					depth_tex = rg.get_texture(depth_buffer);
					blur_tex = rg.get_texture(*blurred_ao_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[AOBlurPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!raw_tex || !depth_tex || !blur_tex) return;

				// Raw AO stays in GENERAL layout; the render graph skips a
				// same-state barrier, so record an explicit memory barrier to
				// order this read after the producing compute dispatch.
				rhi->resource_barrier(cmd, raw_tex, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 0, raw_tex, 0, false, true); // is_general: read raw AO from GENERAL layout
				rhi->cmd_bind_compute_texture(cmd, pipeline, 1, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 2, blur_tex, 0, true);

				struct PushConsts {
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					uint32_t reversed_z;
					uint32_t blur_radius;
				} pc;

				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(depth_desc.width), static_cast<float>(depth_desc.height));
				pc.reversed_z = config.reversed_z ? 1 : 0;
				// Kernel radius measured in half-res AO texels: 5 for GTAO
				// (smooths the 22.5° slice bands on flat walls), 1 for SSAO.
				pc.blur_radius = (config.ao_mode == AOMode::GTAO) ? 5 : 1;

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

				uint32_t gx = (depth_desc.width + 15u) / 16u;
				uint32_t gy = (depth_desc.height + 15u) / 16u;
				rhi->cmd_dispatch(cmd, gx, gy, 1);
			}
		);
	}

	void AOTemporalPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (stored_rhi) {
			auto* pool = stored_rhi->get_resource_pool();
			if (pool) {
				for (auto& tex : history_textures) {
					if (tex) pool->release_texture(tex);
					tex = nullptr;
				}
			}
		}
		has_valid_history = false;
		has_last_view_proj = false;
		history_width = 0;
		history_height = 0;
	}

	void AOTemporalPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;
		stored_rhi = rhi;
		load_shaders_async(asset_manager, { "src/shaders/ao_temporal.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AOTemporal;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline) bud::print("[AOTemporalPass] Shader loaded and pipeline created.");
			});
	}

	RGHandle AOTemporalPass::add_to_graph(RenderGraph& rg, RGHandle raw_ao, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config) {
		if (!pipeline || !raw_ao.is_valid() || !depth_buffer.is_valid() || !config.ao_temporal_enable) return raw_ao;

		auto ao_desc = rg.get_texture_desc(raw_ao);
		if (ao_desc.width == 0 || ao_desc.height == 0) return raw_ao;

		// (Re)create the ping-pong history textures when the AO resolution changes.
		if (stored_rhi && (ao_desc.width != history_width || ao_desc.height != history_height)) {
			auto* pool = stored_rhi->get_resource_pool();
			for (auto& tex : history_textures) {
				if (tex && pool) pool->release_texture(tex);
				tex = nullptr;
			}

			TextureDesc hist_desc = ao_desc;
			hist_desc.is_storage = true;
			for (auto& tex : history_textures) {
				tex = stored_rhi->create_texture(hist_desc, nullptr, 0);
			}
			history_width = ao_desc.width;
			history_height = ao_desc.height;
			has_valid_history = false;
		}

		if (!history_textures[0] || !history_textures[1]) return raw_ao;

		const uint32_t read_idx = history_read_index;
		const uint32_t write_idx = history_read_index ^ 1u;

		// History textures are kept in VK_IMAGE_LAYOUT_GENERAL at all times:
		// sampled with is_general=true and written as a storage image. Importing
		// them as UnorderedAccess (GENERAL) makes the render graph issue no
		// layout transition, so the previous frame's data is preserved.
		RGHandle history_read_h = rg.import_texture("AOHistoryRead", history_textures[read_idx], ResourceState::UnorderedAccess);
		RGHandle history_write_h = rg.import_texture("AOHistoryWrite", history_textures[write_idx], ResourceState::UnorderedAccess);

		// Snapshot camera state NOW (at graph-build time): the graph executes
		// later, by which point last_view_proj would already be rolled forward.
		const bud::math::mat4 prev_view_proj = has_last_view_proj ? last_view_proj : view.view_proj_matrix;
		const uint32_t has_history = (has_valid_history && has_last_view_proj) ? 1u : 0u;

		rg.add_pass("AO Temporal Accumulation",
			[=](RGBuilder& builder) {
				builder.read(raw_ao, ResourceState::ShaderResource);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.read(history_read_h, ResourceState::UnorderedAccess);
				builder.write(history_write_h, ResourceState::UnorderedAccess);
				return history_write_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				Texture* current_tex = nullptr;
				Texture* depth_tex = nullptr;
				Texture* history_tex = nullptr;
				Texture* out_tex = nullptr;
				try {
					current_tex = rg.get_texture(raw_ao);
					depth_tex = rg.get_texture(depth_buffer);
					history_tex = rg.get_texture(history_read_h);
					out_tex = rg.get_texture(history_write_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[AOTemporalPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!current_tex || !depth_tex || !history_tex || !out_tex) return;

				// First frame (or after a resolution change): freshly created
				// history images start in UNDEFINED; bring them into GENERAL so
				// the content-preserving invariant holds from here on.
				if (has_history == 0) {
					rhi->resource_barrier(cmd, history_tex, ResourceState::Undefined, ResourceState::UnorderedAccess);
					rhi->resource_barrier(cmd, out_tex, ResourceState::Undefined, ResourceState::UnorderedAccess);
				}

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 0, current_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 1, history_tex, 0, false, true); // is_general: sample from GENERAL layout
				rhi->cmd_bind_compute_texture(cmd, pipeline, 2, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 3, out_tex, 0, true);

				struct PushConsts {
					bud::math::mat4 inv_proj;
					bud::math::mat4 inv_view;
					bud::math::mat4 prev_view_proj;
					bud::math::vec2 screen_size;
					uint32_t reversed_z;
					uint32_t has_history;
					float noise_rot;
				} pc;

				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.inv_view = bud::math::inverse(view.view_matrix);
				pc.prev_view_proj = prev_view_proj;
				pc.screen_size = bud::math::vec2(static_cast<float>(ao_desc.width), static_cast<float>(ao_desc.height));
				pc.reversed_z = config.reversed_z ? 1 : 0;
				pc.has_history = has_history;
				// Per-frame denoise-tap rotation: ~137.5 deg/frame (golden
				// angle, wrapped) so the integrated footprint is isotropic.
				pc.noise_rot = std::fmod(std::fmod(std::fabs(view.time), 16.0f) * 144.0f, 6.2831853f);

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

				uint32_t gx = (ao_desc.width + 15u) / 16u;
				uint32_t gy = (ao_desc.height + 15u) / 16u;
				rhi->cmd_dispatch(cmd, gx, gy, 1);
			}
		);

		// Roll state forward for the next frame.
		last_view_proj = view.view_proj_matrix;
		has_last_view_proj = true;
		has_valid_history = true;
		history_read_index = write_idx;

		return history_write_h;
	}
}
