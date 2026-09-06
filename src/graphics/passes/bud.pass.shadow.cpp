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

	CSMShadowPass::~CSMShadowPass() {
	}

	void CSMShadowPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (shadow_mesh_pipeline.is_valid()) {
			rhi->destroy_pipeline(shadow_mesh_pipeline);
			shadow_mesh_pipeline.reset();
		}
		if (shadow_visibility_set_layout) {
			rhi->destroy_descriptor_set_layout(shadow_visibility_set_layout);
			shadow_visibility_set_layout = 0;
		}
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

		std::vector<DescriptorBinding> bindings = {
			{1, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT},
			{2, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT | SHADER_STAGE_MESH_BIT},
			{3, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT | SHADER_STAGE_MESH_BIT},
			{4, DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, SHADER_STAGE_TASK_BIT},
			{5, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT},
		};
		shadow_visibility_set_layout = rhi->create_descriptor_set_layout(bindings);

		load_shaders_async(asset_manager, { "src/shaders/visibility.task.spv", "src/shaders/visibility.mesh.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.ts.code = shaders[0];
			desc.ms.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = TextureFormat::Undefined;
			desc.depth_attachment_format = TextureFormat::D32_FLOAT;
			desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
			desc.enable_depth_bias = true;
			desc.vertex_layout = VertexLayoutType::NoVertexInput;
			desc.custom_set_layouts = { shadow_visibility_set_layout };

			shadow_mesh_pipeline = rhi->create_graphics_pipeline(desc);
			if (shadow_mesh_pipeline.is_valid()) {
				bud::print("[CSMShadowPass] VG Shadow Mesh pipeline created: {}", shadow_mesh_pipeline.id);
			}
		});

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
		const ShadowCasterLists& casters,
		bud::graphics::RGHandle rg_indirect_draw,
		std::array<bud::graphics::RGHandle, MAX_CASCADES> rg_csm_visible_pages,
		bud::graphics::BufferHandle vg_shadow_instances)
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

		bool is_vg = config.enable_virtual_geometry && shadow_mesh_pipeline.is_valid();

		auto shadow_map_h = std::make_shared<RGHandle>();

		// Main Shadow Pass (Dynamic Traditional + Static VG)
		return render_graph.add_pass("CSM Shadow",
			[&, shadow_map_h](RGBuilder& builder) {
				*shadow_map_h = builder.create("CSM ShadowMap", desc);
				builder.write(*shadow_map_h, ResourceState::DepthWrite);
				if (rg_indirect_draw.is_valid())
					builder.read(rg_indirect_draw, ResourceState::IndirectArgument);
				if (rg_instance_data.is_valid())
					builder.read(rg_instance_data, ResourceState::ShaderResource);
				for (uint32_t i = 0; i < config.cascade_count; ++i) {
					if (rg_csm_visible_pages[i].is_valid()) {
						builder.read(rg_csm_visible_pages[i], ResourceState::ShaderResource);
					}
				}

				return *shadow_map_h;
			},
			[=, csm_vis = std::move(csm_visible_instances), &render_graph, &render_scene, &meshes, &view, &gpu_scene](RHI* rhi, CommandHandle cmd) {
				auto active_map = render_graph.get_texture(*shadow_map_h);
				if (!active_map.is_valid()) return;

				auto& frame = gpu_scene.get_frame_resources(rhi->get_current_frame_index());

				for (uint32_t i = 0; i < config.cascade_count; ++i) {
					auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
					bud::math::Frustum cascade_view_frustum_dbg;
					cascade_view_frustum_dbg.update(cascade_light_view_proj, config.reversed_z);

					RenderPassBeginInfo info;
					info.depth_attachment = active_map;
					info.clear_depth = true;
					info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;
					info.base_array_layer = i;
					info.layer_count = 1;

					rhi->cmd_begin_render_pass(cmd, info);
					rhi->cmd_set_viewport(cmd, (float)config.shadow_map_size, (float)config.shadow_map_size);
					rhi->cmd_set_scissor(cmd, config.shadow_map_size, config.shadow_map_size);
					// Raster-stage bias ONLY (Vulkan depth-bias units). The receiver-side
					// bias lives in lighting.glsl and is expressed in shadow texels -
					// the two must never share a value, they have different units.
					// Under reversed-z, depth 1.0 is near and 0.0 is far, so pushing the caster
					// away from the light requires negative bias values.
					float bias_constant = config.reversed_z ? -config.shadow_bias_constant : config.shadow_bias_constant;
					float bias_slope = config.reversed_z ? -config.shadow_bias_slope : config.shadow_bias_slope;
					rhi->cmd_set_depth_bias(cmd, bias_constant, config.shadow_bias_clamp, bias_slope);

					// 1. Virtual Geometry Shadow Pass (Mesh Shader path)
					if (is_vg && shadow_mesh_pipeline.is_valid() && rg_csm_visible_pages[i].is_valid()) {
						uint64_t ds = rhi->create_descriptor_set(shadow_visibility_set_layout);
						rhi->update_descriptor_set_buffer(ds, 1, render_graph.get_buffer(rg_csm_visible_pages[i]));
						rhi->update_descriptor_set_buffer(ds, 2, gpu_scene.get_page_pool_buffer());
						// instance_id in the per-cascade visible-page list indexes whatever
						// list the CSM traversal walked, so bind exactly that buffer.
						// (Empty handle = main-view visible instances.)
						{
							BufferHandle shadow_instance_buffer = vg_shadow_instances.is_valid()
								? vg_shadow_instances
								: frame.instance_data;
							rhi->update_descriptor_set_buffer(ds, 3, shadow_instance_buffer);
						}
						TextureHandle hiz_tex = gpu_scene.has_history_hiz()
							? gpu_scene.get_history_hiz(rhi->get_current_frame_index())
							: rhi->get_fallback_texture();
						rhi->update_descriptor_set_image(ds, 4, hiz_tex);
						rhi->update_descriptor_set_buffer(ds, 5, frame.page_cluster_mask);

						rhi->cmd_bind_pipeline(cmd, shadow_mesh_pipeline);
						rhi->cmd_bind_descriptor_set(cmd, shadow_mesh_pipeline, 0, ds);
						rhi->cmd_bind_descriptor_set(cmd, shadow_mesh_pipeline, 1);

						struct VisPush {
							uint32_t cascade_index;
							uint32_t is_shadow_pass;
							uint32_t is_phase2;
							uint32_t enable_hiz;
						} vis_push;
						vis_push.cascade_index = i; // 0 = cascade 0, 1 = cascade 1, etc.
						vis_push.is_shadow_pass = 1;
						vis_push.is_phase2 = 0;
						vis_push.enable_hiz = 0;
						rhi->cmd_push_constants(cmd, shadow_mesh_pipeline, sizeof(VisPush), &vis_push);

						uint32_t vpc = frame.visible_page_capacity;
						if (vpc > 0) {
							rhi->cmd_draw_mesh_tasks(cmd, vpc, 1, 1);
						}
					}

						// 2. Traditional (non page-based) casters. GPU-driven indirect when the
					//    cull buffer is live, otherwise the CPU fallback. The block layout and
					//    the issued sub-range come from the renderer (casters.traditional) -
					//    they used to be re-derived here from unrelated counts, which read the
					//    wrong cascade blocks (corrupting every cascade >= 1) and, in non-VG
					//    mode, drew the VG layer's commands while skipping the traditional
					//    casters entirely.
					const bool use_indirect = frame.csm_indirect_draw.is_valid() && casters.traditional.Valid();
					if (pipeline.is_valid() && (use_indirect || !csm_vis[i].empty())) {
						rhi->cmd_bind_pipeline(cmd, pipeline);
						rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

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

						auto draw_occluder = [&](size_t idx) {
							uint32_t mesh_id = render_scene.mesh_indices[idx];
							if (mesh_id >= meshes.size()) return;
							const auto& mesh = meshes[mesh_id];
							if (!mesh.is_valid() || mesh.is_page_based) return;
							// CPU fallback must honour the same per-instance flag the GPU
							// paths use (RenderScene::INSTANCE_FLAG_NO_CAST_SHADOW == 2).
							if (idx < render_scene.flags.size() && (render_scene.flags[idx] & 2)) return;

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

						if (use_indirect) {
							// The cascade block stride MUST equal what the cull shader was
							// given as total_instances: it writes cascade_count * stride
							// commands. Using any other count silently shifts every
							// cascade >= 1 onto another cascade's data.
							const ShadowCasterRange& tr = casters.traditional;
							const uint32_t block_offset = static_cast<uint32_t>(
								(static_cast<uint64_t>(i) * tr.stride_commands + tr.first_command) * sizeof(bud::graphics::IndirectCommand));
							rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
							rhi->cmd_draw_indexed_indirect(cmd, frame.csm_indirect_draw, block_offset,
								tr.command_count, sizeof(bud::graphics::IndirectCommand));
						} else {
							// Fallback: CPU-driven draw for each visible non-VG instance.
							const auto& visible_instances = csm_vis[i];
							for (size_t k = 0; k < visible_instances.size(); ++k) {
								draw_occluder(visible_instances[k]);
							}
						}
					}

					rhi->cmd_end_render_pass(cmd);
				}
			}
		);
	}

}
