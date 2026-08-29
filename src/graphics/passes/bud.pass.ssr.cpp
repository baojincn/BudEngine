#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

#include <algorithm>

namespace bud::graphics {

	void ScreenSpaceReflectionPass::shutdown(RHI* rhi) {
		if (ssr_pipeline.is_valid() && rhi) {
			rhi->destroy_pipeline(ssr_pipeline);
			ssr_pipeline.reset();
		}
	}

	void ScreenSpaceReflectionPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager)
			return;

		load_shaders_async(asset_manager, { "src/shaders/ssr.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::ScreenSpaceReflections;
			desc.cs.code = shaders[0];
			ssr_pipeline = rhi->create_compute_pipeline(desc);
			if (ssr_pipeline.is_valid())
				bud::print("[ScreenSpaceReflectionPass] SSR compute shader loaded and pipeline created.");
		});
	}

	RGHandle ScreenSpaceReflectionPass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, RGHandle scene_color,
		const SceneView& view, const RenderConfig& config) {
		if (!config.enable_ssr || !depth_buffer.is_valid())
			return {};

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0)
			return {};

		TextureDesc ssr_desc{};
		ssr_desc.width = depth_desc.width;
		ssr_desc.height = depth_desc.height;
		ssr_desc.format = TextureFormat::RGBA16_FLOAT;
		ssr_desc.is_storage = true;

		auto raw_ssr_h = std::make_shared<RGHandle>();

		return rg.add_pass("Screen Space Reflection Pass",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				*raw_ssr_h = builder.create("SSRTexture", ssr_desc);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				if (scene_color.is_valid())
					builder.read(scene_color, ResourceState::ShaderResource);
				builder.write(*raw_ssr_h, ResourceState::UnorderedAccess);
				return *raw_ssr_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				if (!ssr_pipeline.is_valid())
					return;

				TextureHandle depth_tex;
				TextureHandle color_tex;
				TextureHandle ssr_tex;

				try {
					depth_tex = rg.get_texture(depth_buffer);
					if (scene_color.is_valid())
						color_tex = rg.get_texture(scene_color);
					ssr_tex = rg.get_texture(*raw_ssr_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[ScreenSpaceReflectionPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!depth_tex.is_valid() || !ssr_tex.is_valid())
					return;

				rhi->cmd_bind_pipeline(cmd, ssr_pipeline);
				rhi->cmd_bind_compute_texture(cmd, ssr_pipeline, 0, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, ssr_pipeline, 1, color_tex.is_valid() ? color_tex : rhi->get_fallback_texture());
				rhi->cmd_bind_compute_texture(cmd, ssr_pipeline, 2, ssr_tex, 0, true);

				struct SSRPushConsts {
					bud::math::mat4 proj;
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					float max_distance;
					float thickness;
					uint32_t max_steps;
					uint32_t binary_steps;
					float intensity;
					uint32_t reversed_z;
					float roughness_threshold;
					float padding;
				} pc;

				pc.proj = view.proj_matrix;
				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(ssr_desc.width), static_cast<float>(ssr_desc.height));
				pc.max_distance = config.ssr_max_distance;
				pc.thickness = config.ssr_thickness;
				pc.max_steps = config.ssr_max_steps;
				pc.binary_steps = config.ssr_binary_steps;
				pc.intensity = config.ssr_intensity;
				pc.reversed_z = config.reversed_z ? 1 : 0;
				pc.roughness_threshold = 0.6f;
				pc.padding = 0.0f;

				rhi->cmd_push_constants(cmd, ssr_pipeline, sizeof(SSRPushConsts), &pc);
				rhi->cmd_dispatch(cmd, (ssr_desc.width + 15) / 16, (ssr_desc.height + 15) / 16, 1);
			}
		);
	}

} // namespace bud::graphics
