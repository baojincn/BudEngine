#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/physics/bud.cloth.hpp"
#include "src/io/bud.io.hpp"
#include "src/core/bud.logger.hpp"

namespace bud::graphics {

	struct VelocityPushConstants {
		bud::math::mat4 model;
		bud::math::mat4 prev_model;
		uint32_t is_cloth = 0;
		uint32_t cloth_binding_offset = 0;
		uint32_t cloth_vertex_offset = 0;
		uint32_t padding = 0;
	};

	void VelocityPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager)
			return;

		load_shaders_async(asset_manager, { "src/shaders/velocity.vert.spv", "src/shaders/velocity.frag.spv" },
			[this, rhi, config](std::vector<std::vector<char>> shaders) {
				if (shaders.empty() || shaders[0].empty() || shaders[1].empty()) {
					bud::eprint("[VelocityPass] Failed to load velocity shaders.");
					return;
				}

				GraphicsPipelineDesc desc;
				desc.vs.code = shaders[0];
				desc.fs.code = shaders[1];
				desc.depth_test = true;
				desc.depth_write = false;
				desc.depth_compare_op = config.reversed_z ? CompareOp::GreaterEqual : CompareOp::LessEqual;
				desc.cull_mode = CullMode::None;
				desc.color_attachment_format = TextureFormat::RG16_FLOAT;
				desc.vertex_layout = VertexLayoutType::PositionOnly;

				pipeline = rhi->create_graphics_pipeline(desc);
				if (pipeline.is_valid())
					bud::print("[VelocityPass] Velocity pipeline successfully created.");
			});
	}

	RGHandle VelocityPass::add_to_graph(
		RenderGraph& rg,
		RGHandle depth_buffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const GPUScene& gpu_scene,
		BufferHandle mega_vertex_buffer,
		BufferHandle mega_index_buffer,
		const bud::physics::ClothSystem* cloth_system) {

		if (!pipeline.is_valid() || !depth_buffer.is_valid())
			return {};

		const uint32_t width = static_cast<uint32_t>(view.viewport_width);
		const uint32_t height = static_cast<uint32_t>(view.viewport_height);
		if (width == 0 || height == 0)
			return {};

		TextureDesc vel_desc{};
		vel_desc.width = width;
		vel_desc.height = height;
		vel_desc.format = TextureFormat::RG16_FLOAT;
		vel_desc.initial_state = ResourceState::RenderTarget;

		auto rg_velocity_handle = std::make_shared<RGHandle>();

		return rg.add_pass("Velocity Pass",
			[=](RGBuilder& builder) {
				*rg_velocity_handle = builder.create("DynamicVelocity", vel_desc);
				*rg_velocity_handle = builder.write(*rg_velocity_handle, ResourceState::RenderTarget);
				builder.read(depth_buffer, ResourceState::DepthRead);
				return *rg_velocity_handle;
			},
			[=, &rg, &render_scene, &meshes, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid())
					return;

				bool has_cloth_prev = false;
				if (cloth_system && cloth_system->has_cloth()) {
					auto cloth_prev = cloth_system->get_gpu_prev_vertex_positions();
					if (cloth_prev.is_valid()) {
						rhi->update_global_cloth_prev_pos(cloth_prev);
						has_cloth_prev = true;
					}
				}

				RenderPassBeginInfo info;
				info.color_attachments.push_back(rg.get_texture(*rg_velocity_handle));
				info.depth_attachment = rg.get_texture(depth_buffer);
				info.clear_color = true;
				info.clear_color_value = { 0.0f, 0.0f, 0.0f, 0.0f };
				info.clear_depth = false;
				info.depth_read_only = true;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, static_cast<float>(width), static_cast<float>(height));
				rhi->cmd_set_scissor(cmd, width, height);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				const size_t inst_count = render_scene.size();
				for (size_t idx = 0; idx < inst_count; ++idx) {
					const bool is_dynamic = !(render_scene.flags[idx] & RenderScene::INSTANCE_FLAG_STATIC);
					const bool is_cloth = (render_scene.flags[idx] & RenderScene::INSTANCE_FLAG_CLOTH) != 0;

					// Only render dynamic entities (articulated robot links) and XPBD cloth.
					// Static geometry (Sponza) is reprojected analytically in taa.comp.
					if (!is_dynamic && !is_cloth)
						continue;

					const uint32_t mesh_id = render_scene.mesh_indices[idx];
					if (mesh_id >= meshes.size())
						continue;

					const auto& mesh = meshes[mesh_id];
					const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

					VelocityPushConstants pc{};
					if (is_cloth) {
						if (!has_cloth_prev)
							continue;
						pc.model = bud::math::mat4(1.0f);
						pc.prev_model = bud::math::mat4(1.0f);
						pc.is_cloth = 1;
						pc.cloth_binding_offset = cloth_system ? cloth_system->get_mesh_binding_offset(mesh_id) : 0u;
						pc.cloth_vertex_offset = static_cast<uint32_t>(mesh_geometry.vertex_offset);
					} else {
						pc.model = render_scene.world_matrices[idx];
						if (idx < render_scene.prev_world_matrices.size())
							pc.prev_model = render_scene.prev_world_matrices[idx];
						else
							pc.prev_model = pc.model;
						pc.is_cloth = 0;
					}

					rhi->cmd_push_constants(cmd, pipeline, sizeof(VelocityPushConstants), &pc);

					const uint32_t sub_idx = render_scene.submesh_indices[idx];
					if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
						const auto& sub = mesh.submeshes[sub_idx];
						rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, mesh_geometry.vertex_offset, static_cast<uint32_t>(idx));
					} else {
						rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, mesh_geometry.vertex_offset, static_cast<uint32_t>(idx));
					}
				}

				rhi->cmd_end_render_pass(cmd);
			});
	}

}
