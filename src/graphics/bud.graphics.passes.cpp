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
#include "src/graphics/bud.graphics.passes.hpp"

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"
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

	void HiZCullingPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

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
		if (!pipeline) return {};

		return render_graph.add_pass("Hi-Z Culling Pass",
			[=](RGBuilder& builder) {
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.read(hiz_pyramid, ResourceState::UnorderedAccess); // Keep in GENERAL so we can sample it in GENERAL layout
				builder.write(indirect_draw_buffer, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return RGHandle{}; 
			},
			[=, &render_graph, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				auto inst_buf = render_graph.get_buffer(instance_buffer);
				auto ind_buf = render_graph.get_buffer(indirect_draw_buffer);
				auto stat_buf = render_graph.get_buffer(stats_buffer);
				auto depth_tex = render_graph.get_texture(hiz_pyramid);

				if (!inst_buf.is_valid() || !ind_buf.is_valid() || !stat_buf.is_valid() || !depth_tex) {
					static bool printed = false;
					if(!printed) {
						bud::print("[HiZCullingPass] Warning: Missing resources! inst={} ind={} stat={} depth={}", 
							inst_buf.is_valid(), ind_buf.is_valid(), stat_buf.is_valid(), (bool)depth_tex);
						printed = true;
					}
					return;
				}

				// Clear stats buffer (all counters = 0)
				GPUStats zero_stats{};
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_copy_to_buffer(cmd, stat_buf, 0, sizeof(GPUStats), &zero_stats);

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

	void HiZMipPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/hiz_mip.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
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
					rhi->cmd_bind_compute_texture(cmd, pipeline, 3, rg.get_texture(src_handle), (i == 0) ? 0 : (i - 1), false, (i > 0)); // is_general=true when reading from the pyramid (it's in GENERAL layout)
					rhi->cmd_bind_compute_texture(cmd, pipeline, 5, rg.get_texture(current_pyramid), i, true);
					
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
		if (!rhi || !asset_manager) return;
		
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

	void ZPrepass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/zprepass.vert.spv", "src/shaders/zprepass.frag.spv" }, [this, rhi, config](const auto& shaders) {
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
				bud::print("[ZPrepass] Shaders loaded and pipeline created.");
			}
		});
	}

	RGHandle ZPrepass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		size_t instance_count,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer) {
		if (!pipeline) {
			bud::eprint("[ZPrepass] ERROR: Pipeline is null.");
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
			bud::eprint("[ZPrepass] ERROR: RenderScene or sort list is empty.");
			return {};
		}

		const auto* backbuffer_tex = render_graph.get_texture(backbuffer);
		if (!backbuffer_tex || backbuffer_tex->width == 0 || backbuffer_tex->height == 0) {
			return {};
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

		auto depth_h = std::make_shared<RGHandle>();

		return render_graph.add_pass("Z Prepass",
			[=](RGBuilder& builder) {
				*depth_h = builder.create("MainDepth", depth_desc);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *depth_h;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) {
					bud::eprint("[ZPrepass] ERROR: Pipeline is null.");
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

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				for (size_t i = 0; i < draw_count; ++i) {
					const auto& item = sort_list[i];
					uint32_t idx = item.entity_index;

					uint32_t mesh_id = render_scene.mesh_indices[idx];
					uint32_t material_id = render_scene.material_indices[idx];
					const auto& model_matrix = render_scene.world_matrices[idx];

					if (mesh_id >= meshes.size()) continue;
					const auto& mesh = meshes[mesh_id];
					if (!mesh.is_valid()) continue;

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
						rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh.first_index + sub.index_start, mesh.vertex_offset, 0);
					} else {
						push_vars.material_id = material_id;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);
						rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh.first_index, mesh.vertex_offset, 0);
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
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
		if (!rhi || !asset_manager) return;

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
	}

	RGHandle CSMShadowPass::add_to_graph(RenderGraph& render_graph, const SceneView& view, const RenderConfig& config,
		const RenderScene& render_scene,
		const std::vector<RenderMesh>& meshes,
		std::vector<std::vector<uint32_t>> csm_visible_instances,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer)
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

							// Bind global Mega-Buffer once per cascade
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

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

								uint32_t sub_idx = render_scene.submesh_indices[idx];
								if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
									const auto& sub = mesh.submeshes[sub_idx];
									push_consts.material_id = sub.material_id;
									rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
									rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh.first_index + sub.index_start, mesh.vertex_offset, 0);
								}
								else {
									push_consts.material_id = render_scene.material_indices[idx];
									rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
									rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh.first_index, mesh.vertex_offset, 0);
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

					// Bind global Mega-Buffer once per cascade
					rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

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

						uint32_t sub_idx = render_scene.submesh_indices[idx];
						if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[sub_idx];
							push_consts.material_id = sub.material_id;
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh.first_index + sub.index_start, mesh.vertex_offset, 0);
						}
						else {
							push_consts.material_id = render_scene.material_indices[idx];
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh.first_index, mesh.vertex_offset, 0);
						}
					}
					rhi->cmd_end_render_pass(cmd);
				}
			}
		);
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
			if (pipeline) {
				bud::print("[MainPass] Shaders loaded and pipeline created.");
			}
		});
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
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer)
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

		// instance_count = exploded submesh draw count; sort_list is sized to match.
		// max_scene_count guards accessing render_scene arrays, but entity_index in
		// sort_list items are already validated — do NOT clamp draw_count by it.
		const size_t draw_count = std::min(instance_count, sort_list.size());

		uint32_t target_width = backbuffer_tex->width;
		uint32_t target_height = backbuffer_tex->height;

		render_graph.add_pass("Main Lighting Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.read(shadow_map, ResourceState::DepthRead);
				builder.write(depth_buffer, ResourceState::DepthWrite);
				if (config.enable_gpu_driven) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				builder.read(instance_data, ResourceState::ShaderResource);
				return depth_buffer;
			},

			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) {
					bud::eprint("[MainPass] ERROR: Pipeline is null.");
					return;
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

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				// Global Set Bindings
				rhi->update_global_shadow_map(render_graph.get_texture(shadow_map));
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				// Instance Data is already bound to Set 0, Binding 3 by Renderer calling rhi->update_global_instance_data
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				if (config.enable_gpu_driven) {
					rhi->cmd_draw_indexed_indirect(cmd, ind_buf_handle, 0, (uint32_t)draw_count, sizeof(IndirectCommand));
				}
				else {
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;

						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;

						if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[item.submesh_index];
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh.first_index + sub.index_start, mesh.vertex_offset, (uint32_t)i);
						}
						else {
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh.first_index, mesh.vertex_offset, (uint32_t)i);
						}
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
			rhi->destroy_buffer(vertex_buffer);
			current_vertex_buffer_size = 0;
		}
		if (current_index_buffer_size > 0 && rhi) {
			rhi->destroy_buffer(index_buffer);
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
		tex_desc.format = TextureFormat::RGBA8_UNORM;
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
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer)
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
			[=, &render_graph, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
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
					rhi->cmd_draw_indexed_indirect(cmd, ind_buf_handle, 0, (uint32_t)draw_count, sizeof(IndirectCommand));
				}
				else {
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;

						if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[item.submesh_index];
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh.first_index + sub.index_start, mesh.vertex_offset, (uint32_t)i);
						}
						else {
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh.first_index, mesh.vertex_offset, (uint32_t)i);
						}
					}
				}
				rhi->cmd_end_render_pass(cmd);
			}
		);
	}
}
