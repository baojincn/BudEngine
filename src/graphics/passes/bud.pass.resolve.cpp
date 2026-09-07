#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/io/bud.io.hpp"
#include <algorithm>
namespace bud::graphics {
	void ResolvePass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;
		std::vector<DescriptorBinding> bindings = {
		{0, DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, SHADER_STAGE_FRAGMENT_BIT},
		};
		resolve_set_layout = rhi->create_descriptor_set_layout(bindings);
		load_shaders_async(asset_manager, { "src/shaders/resolve.vert.spv", "src/shaders/resolve.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = false;
			desc.depth_write = false;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = TextureFormat::BGRA8_SRGB;
			desc.vertex_layout = VertexLayoutType::NoVertexInput;
			desc.custom_set_layouts = { resolve_set_layout };
			resolve_pipeline = rhi->create_graphics_pipeline(desc);
			if (resolve_pipeline.is_valid()) {
				bud::print("[ResolvePass] Shaders loaded and pipeline created.");
			}
			});
	}
	void ResolvePass::shutdown(RHI* rhi) {
		if (resolve_pipeline.is_valid()) rhi->destroy_pipeline(resolve_pipeline);
		resolve_pipeline.reset();
		if (resolve_set_layout) rhi->destroy_descriptor_set_layout(resolve_set_layout);
		resolve_set_layout = 0;
	}
	RGHandle ResolvePass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer, RGHandle visibility_buffer,
		const SceneView& view, const RenderConfig& config, const GPUScene& gpu_scene, RGHandle shadow_map, RGHandle ao_map, RGHandle ssr_map, RGHandle ssgi_map) {
		if (!resolve_pipeline.is_valid() || !visibility_buffer.is_valid())
			return {};

		auto out_color_h = std::make_shared<RGHandle>(backbuffer);

		return render_graph.add_pass("Resolve Pass",
			[=](RGBuilder& builder) {
				if (!out_color_h->is_valid()) {
					TextureDesc desc{};
					desc.width = static_cast<uint32_t>(view.viewport_width);
					desc.height = static_cast<uint32_t>(view.viewport_height);
					desc.format = TextureFormat::BGRA8_SRGB;
					desc.is_transfer_src = true;
					*out_color_h = builder.create("SceneColor", desc);
				}
				builder.read(visibility_buffer, ResourceState::ShaderResource);
				if (shadow_map.is_valid())
					builder.read(shadow_map, ResourceState::ShaderResource);
				if (ao_map.is_valid())
					builder.read(ao_map, ResourceState::ShaderResource);
				if (ssr_map.is_valid())
					builder.read(ssr_map, ResourceState::ShaderResource);
				if (ssgi_map.is_valid())
					builder.read(ssgi_map, ResourceState::ShaderResource);
				builder.write(*out_color_h, ResourceState::RenderTarget);
				return *out_color_h;
			},
			[=, &render_graph, this](RHI* rhi, CommandHandle cmd) {
				RenderPassBeginInfo rp_info;
				rp_info.color_attachments.push_back(render_graph.get_texture(*out_color_h));
				rp_info.clear_color = true;
				rp_info.clear_color_value = { 0.05f, 0.05f, 0.08f, 1.0f };
				rp_info.render_width = view.viewport_width;
				rp_info.render_height = view.viewport_height;
				rp_info.base_array_layer = 0;
				rp_info.layer_count = 1;
				uint64_t ds = rhi->create_descriptor_set(resolve_set_layout);
				TextureHandle vis_tex = render_graph.get_texture(visibility_buffer);
				rhi->update_descriptor_set_image(ds, 0, vis_tex, 0, DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				if (shadow_map.is_valid()) {
					rhi->update_global_shadow_map(render_graph.get_texture(shadow_map));
				}
				if (ao_map.is_valid()) {
					TextureHandle ao_tex = render_graph.get_texture(ao_map);
					if (ao_tex.is_valid())
						rhi->update_bindless_texture_current_frame(998, ao_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(998, rhi->get_fallback_texture());
				}

				if (ssr_map.is_valid()) {
					TextureHandle ssr_tex = render_graph.get_texture(ssr_map);
					if (ssr_tex.is_valid())
						rhi->update_bindless_texture_current_frame(997, ssr_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(997, rhi->get_fallback_texture());
				}

				if (ssgi_map.is_valid()) {
					TextureHandle ssgi_tex = render_graph.get_texture(ssgi_map);
					if (ssgi_tex.is_valid())
						rhi->update_bindless_texture_current_frame(996, ssgi_tex);
				}
				else {
					rhi->update_bindless_texture_current_frame(996, rhi->get_fallback_texture());
				}

				rhi->cmd_begin_render_pass(cmd, rp_info);
				rhi->cmd_bind_pipeline(cmd, resolve_pipeline);
				rhi->cmd_set_viewport(cmd, (float)view.viewport_width, (float)view.viewport_height);
				rhi->cmd_set_scissor(cmd, view.viewport_width, view.viewport_height);
				rhi->cmd_bind_descriptor_set(cmd, resolve_pipeline, 0, ds);
				rhi->cmd_bind_descriptor_set(cmd, resolve_pipeline, 1);
				rhi->cmd_draw(cmd, 3, 1, 0, 0);
				rhi->cmd_end_render_pass(cmd);
			}
		);
	}
}
