#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

namespace bud::graphics {

	void TAAPass::shutdown(RHI* rhi) {
		if (pipeline.is_valid() && rhi) {
			rhi->destroy_pipeline(pipeline);
			pipeline.reset();
		}
		if (rhi) {
			for (auto& tex : history_textures) {
				if (tex.is_valid()) {
					rhi->destroy_texture(tex);
					tex.reset();
				}
			}
		}
		has_valid_history = false;
		stored_rhi = nullptr;
		history_width = 0;
		history_height = 0;
	}

	void TAAPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager)
			return;

		stored_rhi = rhi;

		load_shaders_async(asset_manager, { "src/shaders/taa.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			if (shaders.empty() || shaders[0].empty()) {
				bud::eprint("[TAAPass] Failed to load taa.comp.spv");
				return;
			}
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::TAA;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline.is_valid())
				bud::print("[TAAPass] TAA compute shader loaded and pipeline created.");
		});
	}

	RGHandle TAAPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle scene_color, RGHandle depth_buffer,
		const SceneView& view, const RenderConfig& config) {
		if (!pipeline.is_valid() || !scene_color.is_valid() || !depth_buffer.is_valid())
			return backbuffer;

		const uint32_t width = static_cast<uint32_t>(view.viewport_width);
		const uint32_t height = static_cast<uint32_t>(view.viewport_height);
		if (width == 0 || height == 0)
			return backbuffer;

		// Recreate ping-pong history textures when resolution changes or not yet allocated
		if (stored_rhi && (history_width != width || history_height != height || !history_textures[0].is_valid())) {
			for (auto& tex : history_textures) {
				if (tex.is_valid()) {
					stored_rhi->destroy_texture(tex);
					tex.reset();
				}
			}

			TextureDesc hist_desc{};
			hist_desc.width = width;
			hist_desc.height = height;
			hist_desc.mips = 1;
			hist_desc.format = TextureFormat::RGBA16_FLOAT;
			hist_desc.is_storage = true;
			hist_desc.is_transfer_src = true;
			hist_desc.initial_state = ResourceState::ShaderResource;

			for (auto& tex : history_textures) {
				tex = stored_rhi->create_texture(hist_desc, nullptr, 0);
			}

			history_width = width;
			history_height = height;
			has_valid_history = false;
		}

		if (!history_textures[0].is_valid() || !history_textures[1].is_valid())
			return backbuffer;

		const uint32_t read_idx = history_read_index;
		const uint32_t write_idx = history_read_index ^ 1u;

		RGHandle rg_history = rg.import_texture("TAAHistoryRead", history_textures[read_idx], has_valid_history ? ResourceState::ShaderResource : ResourceState::Undefined);
		RGHandle rg_current = rg.import_texture("TAAHistoryWrite", history_textures[write_idx], ResourceState::Undefined);

		// Pass 1: Compute TAA Temporal Resolve (writes into history_textures[write_idx])
		rg.add_pass("TAA Resolve Pass",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::Graphics);
				builder.read(scene_color, ResourceState::ShaderResource);
				if (has_valid_history)
					builder.read(rg_history, ResourceState::ShaderResource);
				builder.read(depth_buffer, ResourceState::ShaderResource);
				builder.write(rg_current, ResourceState::UnorderedAccess);
				return rg_current;
			},
			[=, &rg, this](RHI* rhi, CommandHandle cmd) {
				TextureHandle scene_col_tex = rg.get_texture(scene_color);
				TextureHandle depth_tex = rg.get_texture(depth_buffer);
				TextureHandle out_col_tex = rg.get_texture(rg_current);
				TextureHandle hist_col_tex = has_valid_history ? rg.get_texture(rg_history) : TextureHandle{};

				if (!scene_col_tex.is_valid() || !depth_tex.is_valid() || !out_col_tex.is_valid())
					return;

				if (!has_valid_history) {
					rhi->resource_barrier(cmd, out_col_tex, ResourceState::Undefined, ResourceState::UnorderedAccess);
				}

				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 0, scene_col_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 1, hist_col_tex.is_valid() ? hist_col_tex : rhi->get_fallback_texture());
				rhi->cmd_bind_compute_texture(cmd, pipeline, 2, depth_tex);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 3, out_col_tex, 0, true);
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 4);

				struct TAAPushConstants {
					bud::math::vec2 screen_size;
					bud::math::vec2 inv_screen_size;
					float feedback_factor;
					uint32_t reset_history;
					uint32_t reversed_z;
					float jitter_scale;
				} pc;

				pc.screen_size = bud::math::vec2(static_cast<float>(width), static_cast<float>(height));
				pc.inv_screen_size = bud::math::vec2(1.0f / pc.screen_size.x, 1.0f / pc.screen_size.y);
				pc.feedback_factor = config.taa_feedback;
				pc.reset_history = has_valid_history ? 0u : 1u;
				pc.reversed_z = config.reversed_z ? 1u : 0u;
				pc.jitter_scale = config.taa_jitter_scale;

				rhi->cmd_push_constants(cmd, pipeline, sizeof(TAAPushConstants), &pc);

				const uint32_t group_x = (width + 15) / 16;
				const uint32_t group_y = (height + 15) / 16;
				rhi->cmd_dispatch(cmd, group_x, group_y, 1);
			}
		);

		// Pass 2: Blit resolved anti-aliased CurrentSceneColor to Backbuffer
		rg.add_pass("TAA Blit Pass",
			[=](RGBuilder& builder) {
				builder.read(rg_current, ResourceState::TransferSrc);
				builder.write(backbuffer, ResourceState::TransferDst);
				return backbuffer;
			},
			[=, &rg](RHI* rhi, CommandHandle cmd) {
				auto src_tex = rg.get_texture(rg_current);
				auto dst_tex = rg.get_texture(backbuffer);
				if (src_tex.is_valid() && dst_tex.is_valid()) {
					rhi->cmd_blit_image(cmd, src_tex, dst_tex);
				}
			}
		);

		history_read_index ^= 1u;
		has_valid_history = true;

		return backbuffer;
	}

} // namespace bud::graphics
