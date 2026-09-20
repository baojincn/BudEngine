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

	struct VelocityInstanceData {
		bud::math::mat4 model;
		bud::math::mat4 prev_model;
		uint32_t is_cloth;
		uint32_t cloth_binding_offset;
		uint32_t cloth_vertex_offset;
		uint32_t pad;
	};

	void VelocityPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager)
			return;

		std::vector<DescriptorBinding> bindings = {
			{0, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_VERTEX_BIT}
		};
		set_layout = rhi->create_descriptor_set_layout(bindings);

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
				desc.custom_set_layouts = { set_layout };

				pipeline = rhi->create_graphics_pipeline(desc);
				if (pipeline.is_valid())
					bud::print("[VelocityPass] GPU-driven MultiDrawIndirect velocity pipeline successfully created.");
			});
	}

	void VelocityPass::shutdown(RHI* rhi) {
		if (pipeline.is_valid())
			rhi->destroy_pipeline(pipeline);
		pipeline.reset();

		for (auto buf : instance_buffers) {
			if (buf.is_valid())
				rhi->destroy_buffer(buf);
		}
		instance_buffers.clear();

		for (auto buf : indirect_buffers) {
			if (buf.is_valid())
				rhi->destroy_buffer(buf);
		}
		indirect_buffers.clear();
		buffer_capacities.clear();

		if (set_layout)
			rhi->destroy_descriptor_set_layout(set_layout);
		set_layout = 0;
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

				const uint32_t frame_idx = rhi->get_current_image_index();
				if (instance_buffers.size() <= frame_idx) {
					instance_buffers.resize(frame_idx + 1);
					indirect_buffers.resize(frame_idx + 1);
					buffer_capacities.resize(frame_idx + 1, 0);
				}

				const uint32_t needed_capacity = std::max(256u, static_cast<uint32_t>(render_scene.size()));
				if (buffer_capacities[frame_idx] < needed_capacity || !instance_buffers[frame_idx].is_valid()) {
					if (instance_buffers[frame_idx].is_valid())
						rhi->destroy_buffer(instance_buffers[frame_idx]);
					if (indirect_buffers[frame_idx].is_valid())
						rhi->destroy_buffer(indirect_buffers[frame_idx]);

					instance_buffers[frame_idx] = rhi->create_upload_buffer(static_cast<uint64_t>(needed_capacity) * sizeof(VelocityInstanceData));
					indirect_buffers[frame_idx] = rhi->create_upload_buffer(static_cast<uint64_t>(needed_capacity) * sizeof(IndirectCommand));
					buffer_capacities[frame_idx] = needed_capacity;
				}

				auto* inst_buf = rhi->get_buffer(instance_buffers[frame_idx]);
				auto* ind_buf = rhi->get_buffer(indirect_buffers[frame_idx]);
				if (!inst_buf || !inst_buf->mapped_ptr || !ind_buf || !ind_buf->mapped_ptr)
					return;

				auto* mapped_instances = static_cast<VelocityInstanceData*>(inst_buf->mapped_ptr);
				auto* mapped_cmds = static_cast<IndirectCommand*>(ind_buf->mapped_ptr);
				size_t dyn_count = 0;

				const size_t inst_count = render_scene.size();
				for (size_t idx = 0; idx < inst_count; ++idx) {
					const bool is_dynamic = !(render_scene.flags[idx] & RenderScene::INSTANCE_FLAG_STATIC);
					const bool is_cloth = (render_scene.flags[idx] & RenderScene::INSTANCE_FLAG_CLOTH) != 0;

					if (!is_dynamic && !is_cloth)
						continue;

					const uint32_t mesh_id = render_scene.mesh_indices[idx];
					if (mesh_id >= meshes.size())
						continue;

					const auto& mesh = meshes[mesh_id];
					const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

					VelocityInstanceData data{};
					if (is_cloth) {
						if (!has_cloth_prev)
							continue;
						data.model = bud::math::mat4(1.0f);
						data.prev_model = bud::math::mat4(1.0f);
						data.is_cloth = 1;
						data.cloth_binding_offset = cloth_system ? cloth_system->get_mesh_binding_offset(mesh_id) : 0u;
						data.cloth_vertex_offset = static_cast<uint32_t>(mesh_geometry.vertex_offset);
					} else {
						data.model = render_scene.world_matrices[idx];
						if (idx < render_scene.prev_world_matrices.size())
							data.prev_model = render_scene.prev_world_matrices[idx];
						else
							data.prev_model = data.model;
						data.is_cloth = 0;
					}

					mapped_instances[dyn_count] = data;

					const uint32_t sub_idx = render_scene.submesh_indices[idx];
					auto& cmd_out = mapped_cmds[dyn_count];
					cmd_out.instance_count = 1;
					cmd_out.vertex_offset = mesh_geometry.vertex_offset;
					cmd_out.first_instance = static_cast<uint32_t>(dyn_count);

					if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
						const auto& sub = mesh.submeshes[sub_idx];
						cmd_out.index_count = sub.index_count;
						cmd_out.first_index = mesh_geometry.first_index + sub.index_start;
					} else {
						cmd_out.index_count = mesh.index_count;
						cmd_out.first_index = mesh_geometry.first_index;
					}

					++dyn_count;
				}

				RenderPassBeginInfo info;
				info.color_attachments.push_back(rg.get_texture(*rg_velocity_handle));
				info.depth_attachment = rg.get_texture(depth_buffer);
				info.clear_color = true;
				info.clear_color_value = { 0.0f, 0.0f, 0.0f, 0.0f };
				info.clear_depth = false;
				info.depth_read_only = true;

				rhi->cmd_begin_render_pass(cmd, info);

				if (dyn_count > 0) {
					rhi->cmd_bind_pipeline(cmd, pipeline);
					rhi->cmd_set_viewport(cmd, static_cast<float>(width), static_cast<float>(height));
					rhi->cmd_set_scissor(cmd, width, height);

					uint64_t ds = rhi->create_descriptor_set(set_layout);
					rhi->update_descriptor_set_buffer(ds, 0, instance_buffers[frame_idx], DESCRIPTOR_TYPE_STORAGE_BUFFER);

					rhi->cmd_bind_descriptor_set(cmd, pipeline, 0, ds);
					rhi->cmd_bind_descriptor_set(cmd, pipeline, 1); // global set

					rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
					rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

					rhi->cmd_draw_indexed_indirect(cmd, indirect_buffers[frame_idx], 0, static_cast<uint32_t>(dyn_count), sizeof(IndirectCommand));
				}

				rhi->cmd_end_render_pass(cmd);
			});
	}

}
