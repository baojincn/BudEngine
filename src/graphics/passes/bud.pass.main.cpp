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

			if (pipeline.is_valid() && pipeline_wireframe.is_valid()) {
				bud::print("[MainPass] Shaders loaded and pipelines created.");
			}
			});
	}

	void MainPass::shutdown(RHI* rhi) {
		if (pipeline.is_valid()) rhi->destroy_pipeline(pipeline);
		if (pipeline_wireframe.is_valid()) rhi->destroy_pipeline(pipeline_wireframe);
		pipeline.reset();
		pipeline_wireframe.reset();
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

		auto backbuffer_desc = render_graph.get_texture_desc(backbuffer);
		if (backbuffer_desc.width == 0 || backbuffer_desc.height == 0) {
			return;
		}

		const size_t draw_count = std::min(instance_count, sort_list.size());

		uint32_t target_width = backbuffer_desc.width;
		uint32_t target_height = backbuffer_desc.height;

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
				if (!pipeline.is_valid() || !pipeline_wireframe.is_valid()) {
					bud::eprint("[MainPass] ERROR: Pipeline is null.");
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

				bud::graphics::BufferHandle indirect_buffer_handle;
				if (config.enable_gpu_driven) {
					indirect_buffer_handle = render_graph.get_buffer(indirect_draw_buffer);
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
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0);

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
				if (config.enable_gpu_driven) {
					if (indirect_draw_buffer.is_valid() && draw_count > 0) {
						auto page_pool_buf = gpu_scene.get_page_pool_buffer();

						// GPU-driven indirect draw count: the GPU writes up to
						// indirect_capacity commands into the buffer.  Use the
						// full capacity so all cluster-cull outputs are drawn.
						uint32_t gpu_draw_count = static_cast<uint32_t>(draw_count);
						if (config.enable_virtual_geometry) {
							uint32_t frame_idx = rhi->get_current_frame_index();
							gpu_draw_count = std::max(gpu_draw_count,
								gpu_scene.get_frame_resources(frame_idx).indirect_capacity);
						}

						if (split_index > 0) {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
							rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), 0, static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
						}

						if (split_index < gpu_draw_count && page_pool_buf.is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
							rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
							rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), split_index * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(gpu_draw_count - split_index), sizeof(bud::graphics::IndirectCommand));
						}
					}
				}
				else {
					auto page_pool_buf = gpu_scene.get_page_pool_buffer();
					size_t page_backed_draws = 0;
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;

						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						if (mesh.is_page_based && page_pool_buf.is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
							rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
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
				if (config.enable_gpu_driven) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				builder.read(instance_data, ResourceState::ShaderResource);
				return backbuffer;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid()) return;

				bud::graphics::BufferHandle indirect_buffer_handle;
				if (config.enable_gpu_driven) indirect_buffer_handle = render_graph.get_buffer(indirect_draw_buffer);

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

				if (config.enable_gpu_driven && indirect_buffer_handle.is_valid()) {
					auto page_pool_buf = gpu_scene.get_page_pool_buffer();

					uint32_t gpu_draw_count = static_cast<uint32_t>(draw_count);
					if (config.enable_virtual_geometry) {
						uint32_t frame_idx = rhi->get_current_frame_index();
						gpu_draw_count = std::max(gpu_draw_count,
							gpu_scene.get_frame_resources(frame_idx).indirect_capacity);
					}

					if (split_index > 0) {
						rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
						rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						rhi->cmd_draw_indexed_indirect(cmd, indirect_buffer_handle, 0, static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
					}

					if (split_index < gpu_draw_count && page_pool_buf.is_valid()) {
						rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
						rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
						rhi->cmd_draw_indexed_indirect(cmd, indirect_buffer_handle, split_index * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(gpu_draw_count - split_index), sizeof(bud::graphics::IndirectCommand));
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
	}

}
