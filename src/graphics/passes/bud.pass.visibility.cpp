#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/io/bud.io.hpp"
#include <algorithm>
namespace bud::graphics {
	void VisibilityPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager)
			return;
		std::vector<DescriptorBinding> bindings = {
			{1, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT},
			{2, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT | SHADER_STAGE_MESH_BIT},
			{3, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT | SHADER_STAGE_MESH_BIT},
			{4, DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, SHADER_STAGE_TASK_BIT},
			{5, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT},
		};
		visibility_set_layout = rhi->create_descriptor_set_layout(bindings);
		load_shaders_async(asset_manager, { "src/shaders/visibility.task.spv", "src/shaders/visibility.mesh.spv", "src/shaders/visibility.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.ts.code = shaders[0];
			desc.ms.code = shaders[1];
			desc.fs.code = shaders[2];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = TextureFormat::RGBA32_UINT;
			desc.depth_attachment_format = TextureFormat::D32_FLOAT;
			desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
			desc.vertex_layout = VertexLayoutType::NoVertexInput;
			desc.custom_set_layouts = { visibility_set_layout };
			visibility_pipeline = rhi->create_graphics_pipeline(desc);
			desc.wireframe = true;
			visibility_pipeline_wireframe = rhi->create_graphics_pipeline(desc);
		});

		load_shaders_async(asset_manager, { "src/shaders/visibility_indirect.vert.spv", "src/shaders/visibility_indirect.frag.spv" }, [this, rhi, config](std::vector<std::vector<char>> shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = TextureFormat::RGBA32_UINT;
			desc.depth_attachment_format = TextureFormat::D32_FLOAT;
			desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
			desc.vertex_layout = VertexLayoutType::Default;
			visibility_indirect_pipeline = rhi->create_graphics_pipeline(desc);
			desc.wireframe = true;
			visibility_indirect_pipeline_wireframe = rhi->create_graphics_pipeline(desc);
		});
	}
	void VisibilityPass::shutdown(RHI* rhi) {
		if (visibility_pipeline.is_valid())
			rhi->destroy_pipeline(visibility_pipeline);
		if (visibility_pipeline_wireframe.is_valid())
			rhi->destroy_pipeline(visibility_pipeline_wireframe);
		if (visibility_indirect_pipeline.is_valid())
			rhi->destroy_pipeline(visibility_indirect_pipeline);
		if (visibility_indirect_pipeline_wireframe.is_valid())
			rhi->destroy_pipeline(visibility_indirect_pipeline_wireframe);
		visibility_pipeline.reset();
		visibility_pipeline_wireframe.reset();
		visibility_indirect_pipeline.reset();
		visibility_indirect_pipeline_wireframe.reset();
		if (visibility_set_layout)
			rhi->destroy_descriptor_set_layout(visibility_set_layout);
		visibility_descriptor_set = 0;
	}
	RGHandle VisibilityPass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer, RGHandle depth_buffer,
		const SceneView& view, const RenderConfig& config,
		RGHandle rg_visible_pages, RGHandle rg_hiz_pyramid, const GPUScene& gpu_scene,
		const SceneDrawRanges& ranges,
		BufferHandle mega_vertex_buffer,
		BufferHandle mega_index_buffer,
		RGHandle rg_draw,
		RGHandle* out_depth) {
		if (!visibility_pipeline.is_valid() || !rg_visible_pages.is_valid())
			return {};

		uint32_t w = view.viewport_width;
		uint32_t h = view.viewport_height;
		TextureDesc vis_desc;
		vis_desc.format = TextureFormat::RGBA32_UINT;
		vis_desc.width = w;
		vis_desc.height = h;
		TextureDesc depth_desc;
		depth_desc.format = TextureFormat::D32_FLOAT;
		depth_desc.width = w;
		depth_desc.height = h;

		auto vis_h = std::make_shared<RGHandle>();
		auto depth_h = std::make_shared<RGHandle>(depth_buffer);

		auto res = render_graph.add_pass("Visibility Pass (Phase 1)",
			[=](RGBuilder& builder) {
				*vis_h = builder.create("VisibilityBuffer", vis_desc);
				if (!depth_h->is_valid())
					*depth_h = builder.create("DepthBuffer", depth_desc);
				builder.read(rg_visible_pages, ResourceState::ShaderResource);
				if (rg_hiz_pyramid.is_valid())
					builder.read(rg_hiz_pyramid, ResourceState::ShaderResource);
				if (rg_draw.is_valid() && ranges.range_b_count > 0)
					builder.read(rg_draw, ResourceState::IndirectArgument);
				builder.write(*vis_h, ResourceState::RenderTarget);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *vis_h;
			},
			[=, &render_graph, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				TextureHandle visibility_texture_handle = render_graph.get_texture(*vis_h);
				TextureHandle depth_texture_handle = render_graph.get_texture(*depth_h);
				RenderPassBeginInfo rp_info;
				rp_info.color_attachments.push_back(visibility_texture_handle);
				rp_info.depth_attachment = depth_texture_handle;
				rp_info.clear_color = true;
				rp_info.clear_depth = true;
				rp_info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;
				rp_info.render_width = w;
				rp_info.render_height = h;
				rp_info.base_array_layer = 0;
				rp_info.layer_count = 1;

				const auto& frame_res = gpu_scene.get_frame_resources(rhi->get_current_frame_index());

				uint64_t ds = rhi->create_descriptor_set(visibility_set_layout);
				rhi->update_descriptor_set_buffer(ds, 1, render_graph.get_buffer(rg_visible_pages));
				rhi->update_descriptor_set_buffer(ds, 2, gpu_scene.get_page_pool_buffer());
				rhi->update_descriptor_set_buffer(ds, 3, frame_res.instance_data);
				if (rg_hiz_pyramid.is_valid())
					rhi->update_descriptor_set_image(ds, 4, render_graph.get_texture(rg_hiz_pyramid));
				else
					rhi->update_descriptor_set_image(ds, 4, gpu_scene.get_history_hiz(rhi->get_current_frame_index()));
				rhi->update_descriptor_set_buffer(ds, 5, frame_res.page_cluster_mask);
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);

				PipelineHandle active_pipeline = (config.enable_wireframe && visibility_pipeline_wireframe.is_valid()) ? visibility_pipeline_wireframe : visibility_pipeline;
				rhi->cmd_begin_render_pass(cmd, rp_info);
				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_set_viewport(cmd, (float)w, (float)h);
				rhi->cmd_set_scissor(cmd, w, h);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0, ds);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 1);
				struct VisPush {
					uint32_t cascade_index = 0;
					uint32_t is_shadow_pass = 0;
					uint32_t is_phase2 = 0;
					uint32_t enable_hiz = 0;
				} vis_push;
				vis_push.cascade_index = 0;
				vis_push.is_shadow_pass = 0;
				vis_push.is_phase2 = 0;
				vis_push.enable_hiz = (rg_hiz_pyramid.is_valid() && config.enable_hiz_culling) ? 1 : 0;
				rhi->cmd_push_constants(cmd, active_pipeline, sizeof(VisPush), &vis_push);

				// 1. Draw Range A (VG Clusters) via Mesh Shaders
				uint32_t vpc = frame_res.visible_page_capacity;
				if (vpc > 0 && ranges.range_a_count > 0)
					rhi->cmd_draw_mesh_tasks(cmd, vpc, 1, 1);

				// 2. Draw Range B (Traditional Dynamic / Opaque Meshes)
				if (ranges.range_b_count > 0 && mega_vertex_buffer.is_valid() && visibility_indirect_pipeline.is_valid()) {
					PipelineHandle active_indirect = (config.enable_wireframe && visibility_indirect_pipeline_wireframe.is_valid())
						? visibility_indirect_pipeline_wireframe
						: visibility_indirect_pipeline;
					rhi->cmd_bind_pipeline(cmd, active_indirect);
					rhi->cmd_bind_descriptor_set(cmd, active_indirect, 0);
					rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
					rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

					BufferHandle ind_buf = rg_draw.is_valid() ? render_graph.get_buffer(rg_draw) : BufferHandle{};
					if (ind_buf.is_valid()) {
						rhi->cmd_draw_indexed_indirect(cmd, ind_buf,
							static_cast<uint32_t>(ranges.range_a_count * sizeof(bud::graphics::IndirectCommand)),
							static_cast<uint32_t>(ranges.range_b_count),
							sizeof(bud::graphics::IndirectCommand));
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);

		if (out_depth)
			*out_depth = *depth_h;
		return res;
	}

	void VisibilityPass::add_phase2_to_graph(RenderGraph& render_graph,
		RGHandle visibility_buffer,
		RGHandle depth_buffer,
		const SceneView& view,
		const RenderConfig& config,
		RGHandle rg_visible_pages,
		RGHandle rg_current_hiz,
		const GPUScene& gpu_scene) {
		if (!visibility_pipeline.is_valid() || !rg_visible_pages.is_valid() || !visibility_buffer.is_valid() || !depth_buffer.is_valid())
			return;

		uint32_t w = view.viewport_width;
		uint32_t h = view.viewport_height;

		render_graph.add_pass("Visibility Pass (Phase 2)",
			[=](RGBuilder& builder) {
				builder.read(rg_visible_pages, ResourceState::ShaderResource);
				if (rg_current_hiz.is_valid())
					builder.read(rg_current_hiz, ResourceState::ShaderResource);
				builder.write(visibility_buffer, ResourceState::RenderTarget);
				builder.write(depth_buffer, ResourceState::DepthWrite);
				return visibility_buffer;
			},
			[=, &render_graph, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				TextureHandle visibility_texture_handle = render_graph.get_texture(visibility_buffer);
				TextureHandle depth_texture_handle = render_graph.get_texture(depth_buffer);
				RenderPassBeginInfo rp_info;
				rp_info.color_attachments.push_back(visibility_texture_handle);
				rp_info.depth_attachment = depth_texture_handle;
				rp_info.clear_color = false;
				rp_info.clear_depth = false;
				rp_info.render_width = w;
				rp_info.render_height = h;
				rp_info.base_array_layer = 0;
				rp_info.layer_count = 1;

				const auto& frame_res = gpu_scene.get_frame_resources(rhi->get_current_frame_index());

				uint64_t ds = rhi->create_descriptor_set(visibility_set_layout);
				rhi->update_descriptor_set_buffer(ds, 1, render_graph.get_buffer(rg_visible_pages));
				rhi->update_descriptor_set_buffer(ds, 2, gpu_scene.get_page_pool_buffer());
				rhi->update_descriptor_set_buffer(ds, 3, frame_res.instance_data);
				if (rg_current_hiz.is_valid())
					rhi->update_descriptor_set_image(ds, 4, render_graph.get_texture(rg_current_hiz));
				else
					rhi->update_descriptor_set_image(ds, 4, gpu_scene.get_current_hiz(rhi->get_current_frame_index()));
				rhi->update_descriptor_set_buffer(ds, 5, frame_res.page_cluster_mask);
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);

				PipelineHandle active_pipeline = (config.enable_wireframe && visibility_pipeline_wireframe.is_valid()) ? visibility_pipeline_wireframe : visibility_pipeline;
				rhi->cmd_begin_render_pass(cmd, rp_info);
				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_set_viewport(cmd, (float)w, (float)h);
				rhi->cmd_set_scissor(cmd, w, h);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0, ds);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 1);
				struct VisPush {
					uint32_t cascade_index = 0;
					uint32_t is_shadow_pass = 0;
					uint32_t is_phase2 = 1;
					uint32_t enable_hiz = 1;
				} vis_push;
				vis_push.cascade_index = 0;
				vis_push.is_shadow_pass = 0;
				vis_push.is_phase2 = 1;
				vis_push.enable_hiz = (rg_current_hiz.is_valid() && config.enable_hiz_culling) ? 1 : 0;
				rhi->cmd_push_constants(cmd, active_pipeline, sizeof(VisPush), &vis_push);
				uint32_t vpc = frame_res.visible_page_capacity;
				if (vpc > 0)
					rhi->cmd_draw_mesh_tasks(cmd, vpc, 1, 1);
				rhi->cmd_end_render_pass(cmd);
			}
		);
	}

	RGHandle VisibilityPass::add_indirect_to_graph(RenderGraph& render_graph, RGHandle backbuffer, RGHandle depth_buffer,
		const SceneView& view, const RenderConfig& config,
		const RenderScene& render_scene,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		const SceneDrawRanges& ranges,
		RGHandle rg_draw,
		RGHandle rg_instance_data,
		const GPUScene& gpu_scene,
		BufferHandle mega_vertex_buffer,
		BufferHandle mega_index_buffer,
		RGHandle* out_depth) {
		if (!visibility_indirect_pipeline.is_valid())
			return {};

		uint32_t w = view.viewport_width;
		uint32_t h = view.viewport_height;
		TextureDesc vis_desc;
		vis_desc.format = TextureFormat::RGBA32_UINT;
		vis_desc.width = w;
		vis_desc.height = h;
		TextureDesc depth_desc;
		depth_desc.format = TextureFormat::D32_FLOAT;
		depth_desc.width = w;
		depth_desc.height = h;

		auto vis_h = std::make_shared<RGHandle>();
		auto depth_h = std::make_shared<RGHandle>(depth_buffer);

		auto res = render_graph.add_pass("Visibility Indirect Pass",
			[=](RGBuilder& builder) {
				*vis_h = builder.create("VisibilityBuffer", vis_desc);
				if (!depth_h->is_valid())
					*depth_h = builder.create("DepthBuffer", depth_desc);
				if (rg_draw.is_valid())
					builder.read(rg_draw, ResourceState::IndirectArgument);
				if (rg_instance_data.is_valid())
					builder.read(rg_instance_data, ResourceState::ShaderResource);
				builder.write(*vis_h, ResourceState::RenderTarget);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *vis_h;
			},
			[=, &render_graph, &gpu_scene, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {
				TextureHandle visibility_texture_handle = render_graph.get_texture(*vis_h);
				RenderPassBeginInfo rp_info;
				rp_info.color_attachments.push_back(visibility_texture_handle);
				rp_info.depth_attachment = render_graph.get_texture(*depth_h);
				rp_info.clear_color = true;
				rp_info.clear_depth = true;
				rp_info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;
				rp_info.render_width = w;
				rp_info.render_height = h;
				rp_info.base_array_layer = 0;
				rp_info.layer_count = 1;

				PipelineHandle active_pipeline = (config.enable_wireframe && visibility_indirect_pipeline_wireframe.is_valid())
					? visibility_indirect_pipeline_wireframe
					: visibility_indirect_pipeline;

				rhi->cmd_begin_render_pass(cmd, rp_info);
				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_set_viewport(cmd, (float)w, (float)h);
				rhi->cmd_set_scissor(cmd, w, h);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0);

				BufferHandle indirect_buffer_handle = rg_draw.is_valid() ? render_graph.get_buffer(rg_draw) : BufferHandle{};

				if (indirect_buffer_handle.is_valid()) {
					auto page_pool_buf = gpu_scene.get_page_pool_buffer();
					uint32_t frame_idx = rhi->get_current_frame_index();
					const auto& frame_res = gpu_scene.get_frame_resources(frame_idx);
					uint32_t cluster_draw_count = config.enable_virtual_geometry
						? frame_res.visible_cluster_capacity
						: static_cast<uint32_t>(ranges.range_a_count);

					// 1. Draw Range A (VG Clusters)
					if (ranges.range_a_count > 0 && page_pool_buf.is_valid()) {
						rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
						rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
						rhi->cmd_draw_indexed_indirect(cmd, indirect_buffer_handle, 0, cluster_draw_count, sizeof(bud::graphics::IndirectCommand));
					}

					// 2. Draw Range B (Traditional Dynamic / Opaque Meshes)
					if (ranges.range_b_count > 0 && mega_vertex_buffer.is_valid()) {
						rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
						rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						rhi->cmd_draw_indexed_indirect(cmd, indirect_buffer_handle,
							static_cast<uint32_t>(cluster_draw_count * sizeof(bud::graphics::IndirectCommand)),
							static_cast<uint32_t>(ranges.range_b_count),
							sizeof(bud::graphics::IndirectCommand));
					}
				}
				else {
					const size_t opaque_end = ranges.range_a_count + ranges.range_b_count;
					for (size_t i = 0; i < opaque_end && i < sort_list.size(); ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;
						if (idx >= render_scene.mesh_indices.size()) continue;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						if (mesh.is_page_based && gpu_scene.get_page_pool_buffer().is_valid()) {
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

		if (out_depth) {
			*out_depth = *depth_h;
		}
		return res;
	}

}

