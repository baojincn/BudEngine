#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/io/bud.io.hpp"

#include <algorithm>

namespace bud::graphics {

	namespace {
		constexpr uint32_t ao_map_bindless_slot = 998;
		constexpr uint32_t ssr_map_bindless_slot = 997;
		constexpr uint32_t ssgi_map_bindless_slot = 996;
		constexpr uint32_t opaque_scene_color_bindless_slot = 995;
	}

	void ForwardTranslucentPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/forward_main.vert.spv", "src/shaders/forward_main.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = false;
			desc.blending_enable = true;
			desc.blend_mode = BlendMode::PremultipliedAlpha;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.depth_attachment_format = bud::graphics::TextureFormat::D32_FLOAT;
			desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
			desc.enable_depth_bias = false;
			desc.vertex_layout = VertexLayoutType::Default;

			pipeline = rhi->create_graphics_pipeline(desc);

			desc.wireframe = true;
			pipeline_wireframe = rhi->create_graphics_pipeline(desc);

			if (pipeline.is_valid() && pipeline_wireframe.is_valid()) {
				bud::print("[ForwardTranslucentPass] Shaders loaded and pipelines created.");
			}
		});
	}

	void ForwardTranslucentPass::shutdown(RHI* rhi) {
		if (pipeline.is_valid()) rhi->destroy_pipeline(pipeline);
		if (pipeline_wireframe.is_valid()) rhi->destroy_pipeline(pipeline_wireframe);
		pipeline.reset();
		pipeline_wireframe.reset();
	}

	void ForwardTranslucentPass::add_to_graph(RenderGraph& render_graph, RGHandle shadow_map, RGHandle backbuffer, RGHandle depth_buffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		const SceneDrawRanges& ranges,
		RGHandle indirect_draw_buffer,
		RGHandle instance_data,
		const GPUScene& gpu_scene,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer,
		RGHandle ao_map,
		RGHandle ssr_map,
		RGHandle ssgi_map,
		RGHandle opaque_scene_color)
	{
		if (ranges.range_c_count == 0 || sort_list.empty()) {
			return;
		}

		auto backbuffer_desc = render_graph.get_texture_desc(backbuffer);
		if (backbuffer_desc.width == 0 || backbuffer_desc.height == 0) {
			return;
		}

		uint32_t target_width = backbuffer_desc.width;
		uint32_t target_height = backbuffer_desc.height;

		render_graph.add_pass("Forward Translucent Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				if (shadow_map.is_valid()) {
					builder.read(shadow_map, ResourceState::DepthRead);
				}
				builder.read(depth_buffer, ResourceState::DepthRead);
				if (ao_map.is_valid()) {
					builder.read(ao_map, ResourceState::ShaderResource);
				}
				if (ssr_map.is_valid()) {
					builder.read(ssr_map, ResourceState::ShaderResource);
				}
				if (ssgi_map.is_valid()) {
					builder.read(ssgi_map, ResourceState::ShaderResource);
				}
				if (opaque_scene_color.is_valid()) {
					builder.read(opaque_scene_color, ResourceState::ShaderResource);
				}
				if (indirect_draw_buffer.is_valid()) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				if (instance_data.is_valid()) {
					builder.read(instance_data, ResourceState::ShaderResource);
				}
				return backbuffer;
			},

			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid() || !pipeline_wireframe.is_valid()) {
					return;
				}

				PipelineHandle active_pipeline = config.enable_wireframe ? pipeline_wireframe : pipeline;

				if (ao_map.is_valid()) {
					TextureHandle ao_tex = render_graph.get_texture(ao_map);
					if (ao_tex.is_valid()) rhi->update_bindless_texture_current_frame(ao_map_bindless_slot, ao_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(ao_map_bindless_slot, rhi->get_fallback_texture());
				}

				if (ssr_map.is_valid()) {
					TextureHandle ssr_tex = render_graph.get_texture(ssr_map);
					if (ssr_tex.is_valid()) rhi->update_bindless_texture_current_frame(ssr_map_bindless_slot, ssr_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(ssr_map_bindless_slot, rhi->get_fallback_texture());
				}

				if (ssgi_map.is_valid()) {
					TextureHandle ssgi_tex = render_graph.get_texture(ssgi_map);
					if (ssgi_tex.is_valid()) rhi->update_bindless_texture_current_frame(ssgi_map_bindless_slot, ssgi_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(ssgi_map_bindless_slot, rhi->get_fallback_texture());
				}

				if (opaque_scene_color.is_valid()) {
					TextureHandle col_tex = render_graph.get_texture(opaque_scene_color);
					if (col_tex.is_valid()) rhi->update_bindless_texture_current_frame(opaque_scene_color_bindless_slot, col_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(opaque_scene_color_bindless_slot, rhi->get_fallback_texture());
				}

				RenderPassBeginInfo info;
				info.color_attachments.push_back(render_graph.get_texture(backbuffer));
				info.depth_attachment = render_graph.get_texture(depth_buffer);
				info.clear_color = false; // Render forward pass on top of resolved background
				info.clear_depth = false; // Test against existing depth buffer
				info.depth_read_only = true; // Read-only depth attachment for depth test without depth write

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				// Global Set Bindings
				if (shadow_map.is_valid()) {
					rhi->update_global_shadow_map(render_graph.get_texture(shadow_map));
				}
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0);

				// Bind global Mega-Buffer
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				const size_t end_idx = std::min(ranges.range_c_start + ranges.range_c_count, sort_list.size());
				for (size_t i = ranges.range_c_start; i < end_idx; ++i) {
					const auto& item = sort_list[i];
					uint32_t idx = item.entity_index;
					if (idx >= render_scene.mesh_indices.size()) continue;

					uint32_t mesh_id = render_scene.mesh_indices[idx];
					if (mesh_id >= meshes.size()) continue;

					const auto& mesh = meshes[mesh_id];
					if (!mesh.is_valid() || mesh.is_page_based) continue; // Non-VG traditional meshes only

					const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

					struct PushConstants {
						bud::math::mat4 model;
						uint32_t material_id;
						uint32_t is_indirect;
					} pc{};
					pc.model = (idx < render_scene.world_matrices.size()) ? render_scene.world_matrices[idx] : bud::math::mat4(1.0f);
					pc.material_id = (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size())
						? mesh.submeshes[item.submesh_index].material_id
						: (!mesh.submeshes.empty() ? mesh.submeshes[0].material_id : 0);
					pc.is_indirect = 0;
					rhi->cmd_push_constants(cmd, active_pipeline, sizeof(PushConstants), &pc);

					if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
						const auto& sub = mesh.submeshes[item.submesh_index];
						rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, 0);
					}
					else {
						rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, mesh_geometry.vertex_offset, 0);
					}
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
			if (pipeline.is_valid()) {
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
		if (draw_count == 0 || !pipeline.is_valid()) return;

		auto backbuffer_desc = render_graph.get_texture_desc(backbuffer);
		if (backbuffer_desc.width == 0 || backbuffer_desc.height == 0) return;

		uint32_t target_width = backbuffer_desc.width;
		uint32_t target_height = backbuffer_desc.height;

		render_graph.add_pass("Cluster Visualization Pass",
			[=](RGBuilder& builder) {
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.write(depth_buffer, ResourceState::DepthWrite);
				builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				builder.read(instance_data, ResourceState::ShaderResource);
				return backbuffer;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid()) return;

				bud::graphics::BufferHandle indirect_buffer_handle;
				indirect_buffer_handle = render_graph.get_buffer(indirect_draw_buffer);

				RenderPassBeginInfo info;
				info.color_attachments.push_back(render_graph.get_texture(backbuffer));
				info.depth_attachment = render_graph.get_texture(depth_buffer);
				info.clear_color = true;
				info.clear_color_value = { 0.1f, 0.1f, 0.1f, 1.0f };
				info.clear_depth = false;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
				rhi->cmd_draw_indexed_indirect(cmd, indirect_buffer_handle, 0, static_cast<uint32_t>(draw_count), sizeof(bud::graphics::IndirectCommand));

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}

} // namespace bud::graphics
