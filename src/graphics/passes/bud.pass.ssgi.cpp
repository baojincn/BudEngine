#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

#include <algorithm>

namespace bud::graphics {

	void ScreenSpaceGlobalIlluminationPass::shutdown(RHI* rhi) {
		if (stored_rhi) {
			for (auto& tex : history_textures) {
				if (tex.is_valid())
					stored_rhi->destroy_texture(tex);
				tex.reset();
			}
		}
		if (ssgi_pipeline.is_valid() && rhi) {
			rhi->destroy_pipeline(ssgi_pipeline);
			ssgi_pipeline.reset();
		}
		if (denoise_pipeline.is_valid() && rhi) {
			rhi->destroy_pipeline(denoise_pipeline);
			denoise_pipeline.reset();
		}
		if (temporal_pipeline.is_valid() && rhi) {
			rhi->destroy_pipeline(temporal_pipeline);
			temporal_pipeline.reset();
		}
		has_valid_history = false;
		has_last_view = false;
		history_width = 0;
		history_height = 0;
	}

	void ScreenSpaceGlobalIlluminationPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager)
			return;

		stored_rhi = rhi;

		load_shaders_async(asset_manager, { "src/shaders/ssgi.comp.spv", "src/shaders/ssgi_denoise.comp.spv", "src/shaders/ssgi_temporal.comp.spv" },
			[this, rhi](std::vector<std::vector<char>> shaders) {
				ComputePipelineDesc desc;
				desc.layout_kind = ComputePipelineDesc::LayoutKind::ScreenSpaceGlobalIllumination;
				desc.cs.code = shaders[0];
				ssgi_pipeline = rhi->create_compute_pipeline(desc);
				if (ssgi_pipeline.is_valid())
					bud::print("[ScreenSpaceGlobalIlluminationPass] SSGI compute shader loaded and pipeline created.");

				ComputePipelineDesc denoise_desc;
				denoise_desc.layout_kind = ComputePipelineDesc::LayoutKind::SSGIDenoise;
				denoise_desc.cs.code = shaders[1];
				denoise_pipeline = rhi->create_compute_pipeline(denoise_desc);
				if (denoise_pipeline.is_valid())
					bud::print("[ScreenSpaceGlobalIlluminationPass] SSGI denoise compute shader loaded and pipeline created.");

				ComputePipelineDesc temporal_desc;
				temporal_desc.layout_kind = ComputePipelineDesc::LayoutKind::SSGITemporal;
				temporal_desc.cs.code = shaders[2];
				temporal_pipeline = rhi->create_compute_pipeline(temporal_desc);
				if (temporal_pipeline.is_valid())
					bud::print("[ScreenSpaceGlobalIlluminationPass] SSGI temporal accumulation shader loaded and pipeline created.");
			});
	}

	RGHandle ScreenSpaceGlobalIlluminationPass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, RGHandle scene_color,
		const SceneView& view, const RenderConfig& config) {
		if (!config.enable_ssgi || !depth_buffer.is_valid())
			return {};

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0)
			return {};

		// Half-resolution for high performance
		TextureDesc half_ssgi_desc{};
		half_ssgi_desc.width = std::max(1u, depth_desc.width / 2u);
		half_ssgi_desc.height = std::max(1u, depth_desc.height / 2u);
		half_ssgi_desc.format = TextureFormat::RGBA16_FLOAT;
		half_ssgi_desc.is_storage = true;

		// Full-resolution for final upsampled SSGI map
		TextureDesc full_ssgi_desc = depth_desc;
		full_ssgi_desc.format = TextureFormat::RGBA16_FLOAT;
		full_ssgi_desc.is_storage = true;

		// Recreate ping-pong history textures if half-resolution changes
		if (stored_rhi && (half_ssgi_desc.width != history_width || half_ssgi_desc.height != history_height)) {
			for (auto& tex : history_textures) {
				if (tex.is_valid())
					stored_rhi->destroy_texture(tex);
				tex.reset();
			}
			for (auto& tex : history_textures) {
				tex = stored_rhi->create_texture(half_ssgi_desc, nullptr, 0);
			}
			history_width = half_ssgi_desc.width;
			history_height = half_ssgi_desc.height;
			has_valid_history = false;
		}

		auto raw_ssgi_h = std::make_shared<RGHandle>();

		// Pass 1: Half-Resolution SSGI Raymarching
		rg.add_pass("SSGI Raymarch Pass",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				*raw_ssgi_h = builder.create("RawSSGITexture", half_ssgi_desc);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				if (scene_color.is_valid())
					builder.read(scene_color, ResourceState::ShaderResource);
				builder.write(*raw_ssgi_h, ResourceState::UnorderedAccess);
				return *raw_ssgi_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				if (!ssgi_pipeline.is_valid())
					return;

				TextureHandle depth_tex;
				TextureHandle color_tex;
				TextureHandle raw_ssgi_tex;

				try {
					depth_tex = rg.get_texture(depth_buffer);
					if (scene_color.is_valid())
						color_tex = rg.get_texture(scene_color);
					raw_ssgi_tex = rg.get_texture(*raw_ssgi_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[ScreenSpaceGlobalIlluminationPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!depth_tex.is_valid() || !raw_ssgi_tex.is_valid())
					return;

				rhi->cmd_bind_pipeline(cmd, ssgi_pipeline);
				rhi->cmd_bind_compute_texture(cmd, ssgi_pipeline, 0, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, ssgi_pipeline, 1, color_tex.is_valid() ? color_tex : rhi->get_fallback_texture());
				rhi->cmd_bind_compute_texture(cmd, ssgi_pipeline, 2, raw_ssgi_tex, 0, true);

				struct SSGIPushConsts {
					bud::math::mat4 proj;
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					float max_distance;
					float thickness;
					uint32_t ray_count;
					uint32_t max_steps;
					float intensity;
					uint32_t frame_index;
					uint32_t reversed_z;
					float padding;
				} pc;

				pc.proj = view.proj_matrix;
				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(half_ssgi_desc.width), static_cast<float>(half_ssgi_desc.height));
				pc.max_distance = config.ssgi_radius;
				pc.thickness = config.ssgi_thickness;
				pc.ray_count = config.ssgi_ray_count;
				pc.max_steps = config.ssgi_max_steps;
				pc.intensity = config.ssgi_intensity;
				static uint32_t s_frame_idx = 0;
				pc.frame_index = ++s_frame_idx;
				pc.reversed_z = config.reversed_z ? 1 : 0;
				pc.padding = 0.0f;

				rhi->cmd_push_constants(cmd, ssgi_pipeline, sizeof(SSGIPushConsts), &pc);

				uint32_t group_x = (half_ssgi_desc.width + 7) / 8;
				uint32_t group_y = (half_ssgi_desc.height + 7) / 8;
				rhi->cmd_dispatch(cmd, group_x, group_y, 1);
			}
		);

		auto denoise_pre_h = std::make_shared<RGHandle>();

		// Pass 2: Half-Resolution Spatial Pre-Filter (Step Radius 1.5)
		rg.add_pass("SSGI Denoise PreFilter",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				*denoise_pre_h = builder.create("SSGIPingTexture", half_ssgi_desc);
				builder.read(*raw_ssgi_h, ResourceState::ShaderResource);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(*denoise_pre_h, ResourceState::UnorderedAccess);
				return *denoise_pre_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				if (!denoise_pipeline.is_valid())
					return;

				TextureHandle in_tex;
				TextureHandle depth_tex;
				TextureHandle out_tex;

				try {
					in_tex = rg.get_texture(*raw_ssgi_h);
					depth_tex = rg.get_texture(depth_buffer);
					out_tex = rg.get_texture(*denoise_pre_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[ScreenSpaceGlobalIlluminationPass] Denoise PreFilter lookup failed: {}", e.what());
					return;
				}

				if (!in_tex.is_valid() || !depth_tex.is_valid() || !out_tex.is_valid())
					return;

				rhi->cmd_bind_pipeline(cmd, denoise_pipeline);
				rhi->cmd_bind_compute_texture(cmd, denoise_pipeline, 0, in_tex);
				rhi->cmd_bind_compute_texture(cmd, denoise_pipeline, 1, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, denoise_pipeline, 2, out_tex, 0, true);

				struct SSGIDenoisePushConsts {
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					bud::math::vec2 inv_screen_size;
					float depth_threshold;
					float normal_threshold;
					float pass_radius;
					uint32_t reversed_z;
				} pc;

				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(half_ssgi_desc.width), static_cast<float>(half_ssgi_desc.height));
				pc.inv_screen_size = bud::math::vec2(1.0f / pc.screen_size.x, 1.0f / pc.screen_size.y);
				pc.depth_threshold = 0.08f;
				pc.normal_threshold = 0.8f;
				pc.pass_radius = 1.5f;
				pc.reversed_z = config.reversed_z ? 1 : 0;

				rhi->cmd_push_constants(cmd, denoise_pipeline, sizeof(SSGIDenoisePushConsts), &pc);

				uint32_t group_x = (half_ssgi_desc.width + 7) / 8;
				uint32_t group_y = (half_ssgi_desc.height + 7) / 8;
				rhi->cmd_dispatch(cmd, group_x, group_y, 1);
			}
		);

		RGHandle current_temporal_input = *denoise_pre_h;

		// Pass 3: Temporal Accumulation (UE5-style 16-frame history integration with Variance Clamping)
		if (temporal_pipeline.is_valid() && history_textures[0].is_valid() && history_textures[1].is_valid()) {
			const uint32_t read_idx = history_read_index;
			const uint32_t write_idx = history_read_index ^ 1u;

			RGHandle history_read_h = rg.import_texture("SSGIHistoryRead", history_textures[read_idx], has_valid_history ? ResourceState::ShaderResource : ResourceState::Undefined);
			RGHandle history_write_h = rg.import_texture("SSGIHistoryWrite", history_textures[write_idx], has_valid_history ? ResourceState::ShaderResource : ResourceState::Undefined);

			const bud::math::mat4 prev_view_proj = has_last_view ? (last_view_proj) : view.view_proj_matrix;
			const bud::math::mat4 curr_view_to_prev_clip = prev_view_proj * bud::math::inverse(view.view_matrix);
			const uint32_t has_hist = (has_valid_history && has_last_view) ? 1u : 0u;

			auto temporal_out_h = std::make_shared<RGHandle>();

			rg.add_pass("SSGI Temporal Accumulation",
				[=](RGBuilder& builder) {
					builder.set_queue(QueueType::AsyncCompute);
					builder.read(*denoise_pre_h, ResourceState::ShaderResource);
					builder.read(history_read_h, ResourceState::ShaderResource);
					builder.read(depth_buffer, ResourceState::ShaderResource);
					builder.write(history_write_h, ResourceState::UnorderedAccess);
					*temporal_out_h = history_write_h;
					return *temporal_out_h;
				},
				[=, &rg, this](RHI* rhi, CommandHandle cmd) {
					TextureHandle curr_tex = rg.get_texture(*denoise_pre_h);
					TextureHandle hist_tex = rg.get_texture(history_read_h);
					TextureHandle depth_tex = rg.get_texture(depth_buffer);
					TextureHandle out_tex = rg.get_texture(history_write_h);

					if (!curr_tex.is_valid() || !hist_tex.is_valid() || !depth_tex.is_valid() || !out_tex.is_valid())
						return;

					rhi->cmd_bind_pipeline(cmd, temporal_pipeline);
					rhi->cmd_bind_compute_texture(cmd, temporal_pipeline, 0, curr_tex);
					rhi->cmd_bind_compute_texture(cmd, temporal_pipeline, 1, hist_tex);
					rhi->cmd_bind_compute_texture(cmd, temporal_pipeline, 2, depth_tex);
					rhi->cmd_bind_compute_texture(cmd, temporal_pipeline, 3, out_tex, 0, true);

					struct SSGITemporalPushConsts {
						bud::math::mat4 inv_proj;
						bud::math::mat4 curr_view_to_prev_clip;
						bud::math::vec2 screen_size;
						bud::math::vec2 inv_screen_size;
						float temporal_blend;
						uint32_t has_history;
						uint32_t reversed_z;
						float padding;
					} pc;

					pc.inv_proj = bud::math::inverse(view.proj_matrix);
					pc.curr_view_to_prev_clip = curr_view_to_prev_clip;
					pc.screen_size = bud::math::vec2(static_cast<float>(half_ssgi_desc.width), static_cast<float>(half_ssgi_desc.height));
					pc.inv_screen_size = bud::math::vec2(1.0f / pc.screen_size.x, 1.0f / pc.screen_size.y);
					pc.temporal_blend = config.ssgi_temporal_blend;
					pc.has_history = has_hist;
					pc.reversed_z = config.reversed_z ? 1 : 0;
					pc.padding = 0.0f;

					rhi->cmd_push_constants(cmd, temporal_pipeline, sizeof(SSGITemporalPushConsts), &pc);

					uint32_t group_x = (half_ssgi_desc.width + 7) / 8;
					uint32_t group_y = (half_ssgi_desc.height + 7) / 8;
					rhi->cmd_dispatch(cmd, group_x, group_y, 1);
				}
			);

			current_temporal_input = history_write_h;
			history_read_index ^= 1u;
			last_view_proj = view.view_proj_matrix;
			last_view = view.view_matrix;
			has_valid_history = true;
			has_last_view = true;
		}

		auto denoised_final_h = std::make_shared<RGHandle>();

		// Pass 4: Full-Resolution Bilateral Upsample & Final Polish (Step Radius 2.0)
		return rg.add_pass("SSGI Bilateral Upsample",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				*denoised_final_h = builder.create("SSGITexture", full_ssgi_desc);
				builder.read(current_temporal_input, ResourceState::ShaderResource);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(*denoised_final_h, ResourceState::UnorderedAccess);
				return *denoised_final_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				if (!denoise_pipeline.is_valid())
					return;

				TextureHandle in_tex;
				TextureHandle depth_tex;
				TextureHandle out_tex;

				try {
					in_tex = rg.get_texture(current_temporal_input);
					depth_tex = rg.get_texture(depth_buffer);
					out_tex = rg.get_texture(*denoised_final_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[ScreenSpaceGlobalIlluminationPass] Bilateral Upsample lookup failed: {}", e.what());
					return;
				}

				if (!in_tex.is_valid() || !depth_tex.is_valid() || !out_tex.is_valid())
					return;

				rhi->cmd_bind_pipeline(cmd, denoise_pipeline);
				rhi->cmd_bind_compute_texture(cmd, denoise_pipeline, 0, in_tex);
				rhi->cmd_bind_compute_texture(cmd, denoise_pipeline, 1, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, denoise_pipeline, 2, out_tex, 0, true);

				struct SSGIDenoisePushConsts {
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					bud::math::vec2 inv_screen_size;
					float depth_threshold;
					float normal_threshold;
					float pass_radius;
					uint32_t reversed_z;
				} pc;

				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(full_ssgi_desc.width), static_cast<float>(full_ssgi_desc.height));
				pc.inv_screen_size = bud::math::vec2(1.0f / pc.screen_size.x, 1.0f / pc.screen_size.y);
				pc.depth_threshold = 0.08f;
				pc.normal_threshold = 0.8f;
				pc.pass_radius = 2.0f;
				pc.reversed_z = config.reversed_z ? 1 : 0;

				rhi->cmd_push_constants(cmd, denoise_pipeline, sizeof(SSGIDenoisePushConsts), &pc);

				uint32_t group_x = (full_ssgi_desc.width + 7) / 8;
				uint32_t group_y = (full_ssgi_desc.height + 7) / 8;
				rhi->cmd_dispatch(cmd, group_x, group_y, 1);
			}
		);
	}
}
