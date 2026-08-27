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
		if (!rhi || !asset_manager) return;
		std::vector<DescriptorBinding> bindings = {
		{1, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT},
		{2, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT | SHADER_STAGE_MESH_BIT},
		{3, DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, SHADER_STAGE_TASK_BIT | SHADER_STAGE_MESH_BIT},
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
	}
	void VisibilityPass::shutdown(RHI* rhi) {
		if (visibility_pipeline.is_valid()) rhi->destroy_pipeline(visibility_pipeline);
		if (visibility_pipeline_wireframe.is_valid()) rhi->destroy_pipeline(visibility_pipeline_wireframe);
		visibility_pipeline.reset();
		if (visibility_set_layout) rhi->destroy_descriptor_set_layout(visibility_set_layout);
		visibility_descriptor_set = 0;
	}
	RGHandle VisibilityPass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer, RGHandle depth_buffer,
		const SceneView& view, const RenderConfig& config,
		RGHandle rg_visible_pages, RGHandle rg_hiz_pyramid, const GPUScene& gpu_scene,
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

		auto res = render_graph.add_pass("Visibility Pass",
			[=](RGBuilder& builder) {
				*vis_h = builder.create("VisibilityBuffer", vis_desc);
				if (!depth_h->is_valid())
					*depth_h = builder.create("DepthBuffer", depth_desc);
				builder.read(rg_visible_pages, ResourceState::ShaderResource);
				if (rg_hiz_pyramid.is_valid())
					builder.read(rg_hiz_pyramid, ResourceState::ShaderResource);
				builder.write(*vis_h, ResourceState::RenderTarget);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *vis_h;
			},
			[=, &render_graph, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
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
				uint64_t ds = rhi->create_descriptor_set(visibility_set_layout);
				rhi->update_descriptor_set_buffer(ds, 1, render_graph.get_buffer(rg_visible_pages));
				rhi->update_descriptor_set_buffer(ds, 2, gpu_scene.get_page_pool_buffer());
				rhi->update_descriptor_set_buffer(ds, 3, gpu_scene.get_frame_resources(rhi->get_current_frame_index()).instance_data);
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);

				PipelineHandle active_pipeline = (config.enable_wireframe && visibility_pipeline_wireframe.is_valid()) ? visibility_pipeline_wireframe : visibility_pipeline;
				rhi->cmd_begin_render_pass(cmd, rp_info);
				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_set_viewport(cmd, (float)w, (float)h);
				rhi->cmd_set_scissor(cmd, w, h);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 0, ds);
				rhi->cmd_bind_descriptor_set(cmd, active_pipeline, 1);
				uint32_t vpc = gpu_scene.get_frame_resources(rhi->get_current_frame_index()).visible_page_capacity;
				if (vpc > 0)
					rhi->cmd_draw_mesh_tasks(cmd, vpc, 1, 1);
				rhi->cmd_end_render_pass(cmd);
			}
		);
		

		if (out_depth) {
			*out_depth = *depth_h;
		}
		return res;
	}

}
