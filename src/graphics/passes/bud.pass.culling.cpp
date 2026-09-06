#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"

#include <format>
#include <stdexcept>

namespace bud::graphics {

	void InstanceCullingPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("InstanceCullingPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/hiz_cull.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline.is_valid()) {
				bud::print("[InstanceCullingPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle InstanceCullingPass::add_to_graph(RenderGraph& render_graph, RGHandle instance_buffer, RGHandle indirect_draw_buffer, RGHandle stats_buffer, RGHandle hiz_pyramid, const SceneView& view, size_t instance_count) {
		if (!pipeline.is_valid()) {
			std::string err = "InstanceCullingPass::add_to_graph called with invalid pipeline";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return {};
#endif
		}

		return render_graph.add_pass("Hi-Z Culling Pass",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				builder.set_side_effect();
				builder.read(instance_buffer, ResourceState::ShaderResource);
				builder.read(hiz_pyramid, ResourceState::ShaderResource);
				RGHandle new_draw = builder.write(indirect_draw_buffer, ResourceState::UnorderedAccess);
				builder.write(stats_buffer, ResourceState::UnorderedAccess);
				return new_draw;
			},
			[=, &render_graph, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid()) return;

				bud::graphics::BufferHandle inst_buf{};
				bud::graphics::BufferHandle ind_buf{};
				bud::graphics::BufferHandle stat_buf{};
				TextureHandle depth_tex{};
				try {
					inst_buf = render_graph.get_buffer(instance_buffer);
					ind_buf = render_graph.get_buffer(indirect_draw_buffer);
					stat_buf = render_graph.get_buffer(stats_buffer);
					depth_tex = render_graph.get_texture(hiz_pyramid);
				}
				catch (const std::exception& e) {
					bud::eprint("[InstanceCullingPass] Resource lookup failed: {}", e.what());
					return;
				}

				if (!inst_buf.is_valid() || !ind_buf.is_valid() || !stat_buf.is_valid() || !depth_tex.is_valid()) {
					static bool printed = false;
					if (!printed) {
						bud::print("[InstanceCullingPass] Warning: Missing resources! inst={} ind={} stat={} depth={}",
							inst_buf.is_valid(), ind_buf.is_valid(), stat_buf.is_valid(), depth_tex.is_valid());
						printed = true;
					}
					std::string err = std::format("InstanceCullingPass missing resources: inst={} ind={} stat={} depth={}", inst_buf.is_valid(), ind_buf.is_valid(), stat_buf.is_valid(), depth_tex.is_valid());
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				// Clear stats buffer (all counters = 0)
				rhi->resource_barrier(cmd, stat_buf, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_fill_buffer(cmd, stat_buf, 0, sizeof(bud::graphics::GPUStats), 0);

				// Barrier: ensure the fill write is visible to the compute shader
				rhi->resource_barrier(cmd, stat_buf, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				rhi->cmd_bind_pipeline(cmd, pipeline);

				rhi->cmd_bind_storage_buffer(cmd, pipeline, 0, inst_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 1, ind_buf);
				rhi->cmd_bind_storage_buffer(cmd, pipeline, 2, stat_buf);
				rhi->cmd_bind_compute_texture(cmd, pipeline, 3, depth_tex, ALL_MIPS, false, false);
				rhi->cmd_bind_compute_ubo(cmd, pipeline, 4);

				struct PushConsts {
					uint32_t instanceCount;
				} pc;
				pc.instanceCount = static_cast<uint32_t>(instance_count);

				rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &pc);

				// Dispatch 1 thread per instance
				uint32_t group_x = (static_cast<uint32_t>(instance_count) + 255) / 256;
				rhi->cmd_dispatch(cmd, group_x, 1, 1);
			}
		);
	}

}
