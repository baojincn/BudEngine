#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/io/bud.io.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <algorithm>

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
	}

	void CSMShadowPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (rhi && static_cache_texture.is_valid()) {
			rhi->destroy_texture(static_cache_texture);
			static_cache_texture.reset();
		}

		static_cache_texture.reset();
		cache_initialized = false;
		has_last_view_proj = false;
		has_last_cascade_proj = false;
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
			if (pipeline.is_valid()) {
				bud::print("[CSMShadowPass] Shaders loaded and pipeline created: {}", pipeline.id);
			}
			});
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
		size_t split_index,
		bud::graphics::RGHandle rg_indirect_draw,
		bud::graphics::RGHandle rg_static_indirect_draw)
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
		desc.initial_state = ResourceState::DepthRead;

		if (!config.cache_shadows && static_cache_texture.is_valid()) {
			shutdown(stored_rhi);
		}

		bool light_changed = bud::math::length(view.light_dir - last_light_dir) > 0.001f;
		bool cascades_changed = false;
		for (uint32_t i = 0; i < cascade_count; ++i) {
			if (!has_last_cascade_proj || !mat4_nearly_equal(view.cascade_view_proj_matrices[i], last_cascade_view_proj[i])) {
				cascades_changed = true;
				break;
			}
		}
		bool config_changed = !has_last_config || !shadow_config_equal(config, last_config);
		bool need_update = !cache_initialized || light_changed || cascades_changed || config_changed || !config.cache_shadows;

		if (config.cache_shadows) {
			bool needs_recreate = !static_cache_texture.is_valid();
			if (stored_rhi && !needs_recreate) {
				auto current_desc = stored_rhi->get_texture_desc(static_cache_texture);
				needs_recreate = current_desc.width != desc.width
					|| current_desc.height != desc.height
					|| current_desc.format != desc.format
					|| current_desc.type != desc.type
					|| current_desc.array_layers != desc.array_layers;
			}

			if (stored_rhi && needs_recreate) {
				if (static_cache_texture.is_valid()) {
					stored_rhi->destroy_texture(static_cache_texture);
					static_cache_texture.reset();
				}
				static_cache_texture = stored_rhi->create_texture(desc, nullptr, 0);
				need_update = true;
				cache_initialized = false;
			}
		}

		auto shadow_map_h = std::make_shared<RGHandle>();

		// Whether the static cache texture is available (used by the cull pass
		// to decide if statics can be skipped for the dynamic CSM draw).
		bool valid_cache = config.cache_shadows && static_cache_texture.is_valid() && cache_initialized;

		RGHandle static_cache_h;
		if (config.cache_shadows && static_cache_texture.is_valid()) {
			static_cache_h = render_graph.import_texture("CSM StaticCache", static_cache_texture, ResourceState::DepthRead);

			if (need_update) {
				render_graph.add_pass("CSM Static Cache Update",
					[&](RGBuilder& builder) {
						builder.write(static_cache_h, ResourceState::DepthWrite);
						if (rg_static_indirect_draw.is_valid())
							builder.read(rg_static_indirect_draw, ResourceState::IndirectArgument);
						if (rg_instance_data.is_valid())
							builder.read(rg_instance_data, ResourceState::ShaderResource);
					},
					[=, this, &render_graph, &render_scene, &meshes, &view, &gpu_scene](RHI* rhi, CommandHandle cmd) {
						if (!pipeline.is_valid()) return;

						auto static_map = render_graph.get_texture(static_cache_h);
						for (uint32_t i = 0; i < config.cascade_count; ++i) {
							auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
							bud::math::Frustum cascade_view_frustum;
							cascade_view_frustum.update(cascade_light_view_proj, config.reversed_z);

							RenderPassBeginInfo info;
							info.depth_attachment = static_map;
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
								uint32_t material_id;
								uint32_t use_gpu_driven;
								uint32_t page_slot;
							} push_consts;

							push_consts.light_view_proj = cascade_light_view_proj;
							push_consts.model = bud::math::mat4(1.0f);
							push_consts.material_id = 0;
							push_consts.use_gpu_driven = 1;
							push_consts.page_slot = ~0u;

							auto page_pool_buf = gpu_scene.get_page_pool_buffer();

							auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
							if (frame.csm_static_indirect_draw.is_valid()) {
								rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);

								if (split_index > 0) {
									rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
									rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
									rhi->cmd_draw_indexed_indirect(cmd, frame.csm_static_indirect_draw, i * static_cast<uint32_t>(instance_count) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
								}

								if (split_index < instance_count && page_pool_buf.is_valid()) {
									rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
									rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
									rhi->cmd_draw_indexed_indirect(cmd, frame.csm_static_indirect_draw, (i * static_cast<uint32_t>(instance_count) + split_index) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(instance_count - split_index), sizeof(bud::graphics::IndirectCommand));
								}
							}
							rhi->cmd_end_render_pass(cmd);
						}

						cache_initialized = true;
						last_light_dir = view.light_dir;
						last_view_proj = view.view_proj_matrix;
						for (uint32_t i = 0; i < config.cascade_count; ++i)
							last_cascade_view_proj[i] = view.cascade_view_proj_matrices[i];
						has_last_cascade_proj = true;
						last_config = config;
						has_last_view_proj = true;
						has_last_config = true;
					}
				);
			}
		}

		// Main Shadow Pass (Dynamic + Copy)
		return render_graph.add_pass("CSM Shadow",
			[&, shadow_map_h](RGBuilder& builder) {
				*shadow_map_h = builder.create("CSM ShadowMap", desc);
				builder.write(*shadow_map_h, ResourceState::DepthWrite);
				if (valid_cache)
					builder.read(static_cache_h, ResourceState::DepthRead);
				if (rg_indirect_draw.is_valid())
					builder.read(rg_indirect_draw, ResourceState::IndirectArgument);
				if (rg_instance_data.is_valid())
					builder.read(rg_instance_data, ResourceState::ShaderResource);

				return *shadow_map_h;
			},
			[=, csm_vis = std::move(csm_visible_instances), &render_graph, &render_scene, &meshes, &view, &gpu_scene](RHI* rhi, CommandHandle cmd) {
				auto active_map = render_graph.get_texture(*shadow_map_h);
				if (!pipeline.is_valid()) {
					if (active_map.is_valid()) {
						rhi->resource_barrier(cmd, active_map, ResourceState::DepthWrite, ResourceState::DepthRead);
					}
					return;
				}
				bool did_copy = false;

				if (valid_cache && cache_initialized) {
					TextureHandle static_map;
					try {
						static_map = render_graph.get_texture(static_cache_h);
					}
					catch (const std::exception& e) {
						bud::eprint("[CSMShadowPass] failed to get static cache texture for copy: {}", e.what());
					}
					if (static_map.is_valid()) {
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
					push_consts.use_gpu_driven = 1;
					push_consts.page_slot = ~0u;

					const auto page_pool_buf = gpu_scene.get_page_pool_buffer();
					auto draw_occluder = [&](size_t idx, bool skip_cached_static) {
						bool is_static = (render_scene.flags[idx] & 1) != 0;
						if (skip_cached_static && is_static) return;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) return;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid() || mesh.is_page_based) return;

						// Culling
						const auto& model_matrix = render_scene.world_matrices[idx];
						bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
						if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) return;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						push_consts.use_gpu_driven = 0;
						push_consts.model = model_matrix;
						push_consts.page_slot = ~0u;

						rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
						rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

						uint32_t sub_idx = render_scene.submesh_indices[idx];
						if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
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

					auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());
					if (frame.csm_indirect_draw.is_valid()) {
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);

						if (split_index > 0) {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
							rhi->cmd_draw_indexed_indirect(cmd, frame.csm_indirect_draw, i * static_cast<uint32_t>(instance_count) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
						}

						if (split_index < instance_count && page_pool_buf.is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
							rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
							rhi->cmd_draw_indexed_indirect(cmd, frame.csm_indirect_draw, (i * static_cast<uint32_t>(instance_count) + split_index) * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(instance_count - split_index), sizeof(bud::graphics::IndirectCommand));
						}
					} else {
						// Fallback: CPU-driven draw for each visible instance.
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

}
