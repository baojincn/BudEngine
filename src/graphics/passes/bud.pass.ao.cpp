#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

#include <cmath>
#include <algorithm>

namespace bud::graphics {

	void AmbientOcclusionPass::shutdown(RHI* rhi) {
		if (ssao_pipeline.is_valid() && rhi) { rhi->destroy_pipeline(ssao_pipeline); ssao_pipeline.reset(); }
		if (gtao_pipeline.is_valid() && rhi) { rhi->destroy_pipeline(gtao_pipeline); gtao_pipeline.reset(); }
	}

	void AmbientOcclusionPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/ssao.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AmbientOcclusion;
			desc.cs.code = shaders[0];
			ssao_pipeline = rhi->create_compute_pipeline(desc);
			if (ssao_pipeline.is_valid()) bud::print("[AmbientOcclusionPass] SSAO shader loaded and pipeline created.");
			});

		load_shaders_async(asset_manager, { "src/shaders/gtao.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AmbientOcclusion;
			desc.cs.code = shaders[0];
			gtao_pipeline = rhi->create_compute_pipeline(desc);
			if (gtao_pipeline.is_valid()) bud::print("[AmbientOcclusionPass] GTAO shader loaded and pipeline created.");
			});
	}

	RGHandle AmbientOcclusionPass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config) {
		if (config.ao_mode == AOMode::Disabled || !depth_buffer.is_valid()) return {};

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0) return {};

		// Evaluate AO at half resolution by default and let the AO blur pass
		// depth-aware-upsample it back to full resolution.
		const uint32_t downscale = config.ao_half_res ? 2u : 1u;
		TextureDesc ao_desc{};
		ao_desc.width = std::max(1u, depth_desc.width / downscale);
		ao_desc.height = std::max(1u, depth_desc.height / downscale);
		ao_desc.format = TextureFormat::R32_FLOAT;
		ao_desc.is_storage = true;

		auto raw_ao_h = std::make_shared<RGHandle>();

		return rg.add_pass("Ambient Occlusion Pass",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				*raw_ao_h = builder.create("RawAOTexture", ao_desc);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(*raw_ao_h, ResourceState::UnorderedAccess);
				return *raw_ao_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				PipelineHandle active_pipeline = (config.ao_mode == AOMode::GTAO) ? gtao_pipeline : ssao_pipeline;
				if (!active_pipeline.is_valid()) return;

				TextureHandle depth_tex;
				TextureHandle ao_tex;
				try {
					depth_tex = rg.get_texture(depth_buffer);
					ao_tex = rg.get_texture(*raw_ao_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[AmbientOcclusionPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!depth_tex.is_valid() || !ao_tex.is_valid()) return;

				rhi->cmd_bind_pipeline(cmd, active_pipeline);
				rhi->cmd_bind_compute_texture(cmd, active_pipeline, 0, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, active_pipeline, 1, ao_tex, 0, true);

				struct PushConsts {
					bud::math::mat4 proj;
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					float radius;
					float intensity;
					uint32_t sample_count;
					uint32_t reversed_z;
					float time;
				} pc;

				pc.proj = view.proj_matrix;
				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(ao_desc.width), static_cast<float>(ao_desc.height));
				pc.radius = config.ao_radius;
				pc.intensity = config.ao_intensity;
				pc.sample_count = config.ao_sample_count;
				pc.reversed_z = config.reversed_z ? 1 : 0;
				pc.time = std::fmod(std::fmod(std::fabs(view.time), 16.0f) * 23.0f, 1.0f);

				rhi->cmd_push_constants(cmd, active_pipeline, sizeof(PushConsts), &pc);

				uint32_t gx = (ao_desc.width + 15u) / 16u;
				uint32_t gy = (ao_desc.height + 15u) / 16u;
				rhi->cmd_dispatch(cmd, gx, gy, 1);
			}
		);
	}

	void AOBlurPass::shutdown(RHI* rhi) {
		if (pipeline.is_valid() && rhi) { rhi->destroy_pipeline(pipeline); pipeline.reset(); }
	}

	void AOBlurPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;

		load_shaders_async(asset_manager, { "src/shaders/ao_blur.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AOBlur;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline.is_valid()) bud::print("[AOBlurPass] Shader loaded and pipeline created.");
			});
	}

	RGHandle AOBlurPass::add_to_graph(RenderGraph& rg, RGHandle raw_ao, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config) {
		if (!pipeline.is_valid() || !raw_ao.is_valid() || !depth_buffer.is_valid() || !config.ao_blur_enable) return raw_ao;

		auto raw_desc = rg.get_texture_desc(raw_ao);
		if (raw_desc.width == 0 || raw_desc.height == 0) return raw_ao;

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0) return raw_ao;

		TextureDesc blur_desc{};
		blur_desc.width = depth_desc.width;
		blur_desc.height = depth_desc.height;
		blur_desc.format = raw_desc.format;
		blur_desc.is_storage = true;
		auto blurred_ao_h = std::make_shared<RGHandle>();

		return rg.add_pass("AO Blur Pass",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				*blurred_ao_h = builder.create("BlurredAOTexture", blur_desc);
				builder.read(raw_ao, ResourceState::UnorderedAccess);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(*blurred_ao_h, ResourceState::UnorderedAccess);
				return *blurred_ao_h;
			},
			[=, &rg](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid()) return;

				TextureHandle raw_tex;
				TextureHandle depth_tex;
				TextureHandle blur_tex;
				try {
					raw_tex = rg.get_texture(raw_ao);
					depth_tex = rg.get_texture(depth_buffer);
					blur_tex = rg.get_texture(*blurred_ao_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[AOBlurPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!raw_tex.is_valid() || !depth_tex.is_valid() || !blur_tex.is_valid()) return;

				rhi->resource_barrier(cmd, raw_tex, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 0, raw_tex, 0, false, true); // is_general: read raw AO from GENERAL layout
				rhi->cmd_bind_compute_texture(cmd, pipeline, 1, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 2, blur_tex, 0, true);

				struct PushConsts {
					bud::math::mat4 inv_proj;
					bud::math::vec2 screen_size;
					uint32_t reversed_z;
					uint32_t blur_radius;
				} pc;

				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.screen_size = bud::math::vec2(static_cast<float>(depth_desc.width), static_cast<float>(depth_desc.height));
				pc.reversed_z = config.reversed_z ? 1 : 0;
				pc.blur_radius = (config.ao_mode == AOMode::GTAO) ? 5 : 1;

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

				uint32_t gx = (depth_desc.width + 15u) / 16u;
				uint32_t gy = (depth_desc.height + 15u) / 16u;
				rhi->cmd_dispatch(cmd, gx, gy, 1);
			}
		);
	}

	void AOTemporalPass::shutdown(RHI* rhi) {
		RenderPass::shutdown(rhi);
		if (stored_rhi) {
			for (auto& tex : history_textures) {
				if (tex.is_valid()) stored_rhi->destroy_texture(tex);
				tex.reset();
			}
		}
		has_valid_history = false;
		has_last_view_proj = false;
		history_width = 0;
		history_height = 0;
	}

	void AOTemporalPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) return;
		stored_rhi = rhi;
		load_shaders_async(asset_manager, { "src/shaders/ao_temporal.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::AOTemporal;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline.is_valid()) bud::print("[AOTemporalPass] Shader loaded and pipeline created.");
			});
	}

	RGHandle AOTemporalPass::add_to_graph(RenderGraph& rg, RGHandle raw_ao, RGHandle depth_buffer, const SceneView& view, const RenderConfig& config) {
		if (!pipeline.is_valid() || !raw_ao.is_valid() || !depth_buffer.is_valid() || !config.ao_temporal_enable) return raw_ao;

		auto ao_desc = rg.get_texture_desc(raw_ao);
		if (ao_desc.width == 0 || ao_desc.height == 0) return raw_ao;

		// (Re)create the ping-pong history textures when the AO resolution changes.
		if (stored_rhi && (ao_desc.width != history_width || ao_desc.height != history_height)) {
			for (auto& tex : history_textures) {
				if (tex.is_valid()) stored_rhi->destroy_texture(tex);
				tex.reset();
			}

			TextureDesc hist_desc = ao_desc;
			hist_desc.is_storage = true;
			for (auto& tex : history_textures) {
				tex = stored_rhi->create_texture(hist_desc, nullptr, 0);
			}
			history_width = ao_desc.width;
			history_height = ao_desc.height;
			has_valid_history = false;
		}

		if (!history_textures[0].is_valid() || !history_textures[1].is_valid()) return raw_ao;

		const uint32_t read_idx = history_read_index;
		const uint32_t write_idx = history_read_index ^ 1u;

		RGHandle history_read_h = rg.import_texture("AOHistoryRead", history_textures[read_idx], ResourceState::UnorderedAccess);
		RGHandle history_write_h = rg.import_texture("AOHistoryWrite", history_textures[write_idx], ResourceState::UnorderedAccess);

		const bud::math::mat4 prev_view_proj = has_last_view_proj ? last_view_proj : view.view_proj_matrix;
		const uint32_t has_history = (has_valid_history && has_last_view_proj) ? 1u : 0u;

		rg.add_pass("AO Temporal Accumulation",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				builder.read(raw_ao, ResourceState::ShaderResource);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.read(history_read_h, ResourceState::UnorderedAccess);
				builder.write(history_write_h, ResourceState::UnorderedAccess);
				return history_write_h;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				TextureHandle current_tex;
				TextureHandle depth_tex;
				TextureHandle history_tex;
				TextureHandle out_tex;
				try {
					current_tex = rg.get_texture(raw_ao);
					depth_tex = rg.get_texture(depth_buffer);
					history_tex = rg.get_texture(history_read_h);
					out_tex = rg.get_texture(history_write_h);
				}
				catch (const std::exception& e) {
					bud::eprint("[AOTemporalPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!current_tex.is_valid() || !depth_tex.is_valid() || !history_tex.is_valid() || !out_tex.is_valid()) return;

				if (has_history == 0) {
					rhi->resource_barrier(cmd, history_tex, ResourceState::Undefined, ResourceState::UnorderedAccess);
					rhi->resource_barrier(cmd, out_tex, ResourceState::Undefined, ResourceState::UnorderedAccess);
				}

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 0, current_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 1, history_tex, 0, false, true); // is_general: sample from GENERAL layout
				rhi->cmd_bind_compute_texture(cmd, pipeline, 2, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 3, out_tex, 0, true);

				struct PushConsts {
					bud::math::mat4 inv_proj;
					bud::math::mat4 inv_view;
					bud::math::mat4 prev_view_proj;
					bud::math::vec2 screen_size;
					uint32_t reversed_z;
					uint32_t has_history;
					float noise_rot;
				} pc;

				pc.inv_proj = bud::math::inverse(view.proj_matrix);
				pc.inv_view = bud::math::inverse(view.view_matrix);
				pc.prev_view_proj = prev_view_proj;
				pc.screen_size = bud::math::vec2(static_cast<float>(ao_desc.width), static_cast<float>(ao_desc.height));
				pc.reversed_z = config.reversed_z ? 1 : 0;
				pc.has_history = has_history;
				pc.noise_rot = std::fmod(std::fmod(std::fabs(view.time), 16.0f) * 144.0f, 6.2831853f);

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

				uint32_t gx = (ao_desc.width + 15u) / 16u;
				uint32_t gy = (ao_desc.height + 15u) / 16u;
				rhi->cmd_dispatch(cmd, gx, gy, 1);
			}
		);

		last_view_proj = view.view_proj_matrix;
		has_last_view_proj = true;
		has_valid_history = true;
		history_read_index = write_idx;

		return history_write_h;
	}

}
