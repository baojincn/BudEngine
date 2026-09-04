#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/io/bud.io.hpp"

namespace bud::graphics {

	// ---------------------------------------------------------
	// HierarchyTraversalPass
	// ---------------------------------------------------------
	HierarchyTraversalPass::~HierarchyTraversalPass() = default;

	void HierarchyTraversalPass::shutdown(RHI* rhi) {
		if (hierarchy_traversal_pipeline.is_valid()) {
			rhi->destroy_pipeline(hierarchy_traversal_pipeline);
			hierarchy_traversal_pipeline.reset();
		}
	}

	void HierarchyTraversalPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		load_shaders_async(asset_manager, { "src/shaders/hierarchy_traversal.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::HierarchyTraversal;
			desc.cs.code = shaders[0];
			hierarchy_traversal_pipeline = rhi->create_compute_pipeline(desc);
			if (hierarchy_traversal_pipeline.is_valid()) bud::print("[HierarchyTraversalPass] Shader loaded and pipeline created.");
		});
	}

	RGHandle HierarchyTraversalPass::add_to_graph(
		RenderGraph& rg,
		const SceneView& view,
		const RenderConfig& config,
		const RenderScene& render_scene,
		const std::vector<RenderMesh>& meshes,
		size_t instance_count,
		const GPUScene& gpu_scene,
		uint32_t current_frame,
		uint32_t cascade_index,
		float lod_error_scale,
		float ortho_extent,
		BufferHandle target_visible_pages,
		const std::string& pass_name,
		BufferHandle source_instances)
	{
		if (!hierarchy_traversal_pipeline.is_valid())
			return {};

		const auto& frame = gpu_scene.get_frame_resources(current_frame);
		BufferHandle active_visible_pages = target_visible_pages.is_valid() ? target_visible_pages : frame.visible_pages;

		if (!active_visible_pages.is_valid())
			return {};

		std::string buf_name = (cascade_index == 0) ? "VisiblePages" : ("CSMVisiblePages_" + std::to_string(cascade_index - 1));
		RGHandle rg_visible_pages = rg.import_buffer(buf_name, active_visible_pages, ResourceState::UnorderedAccess);

		return rg.add_pass(pass_name,
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				builder.write(rg_visible_pages, ResourceState::UnorderedAccess);
				return rg_visible_pages;
			},
			[=, &rg, &view, &config, &render_scene, &meshes, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				const auto& frame = gpu_scene.get_frame_resources(current_frame);
				BufferHandle actual_vp = target_visible_pages.is_valid() ? target_visible_pages : frame.visible_pages;
				uint32_t zero = 0;
				
				// Clear atomic counters for GPU driven pipeline
				rhi->resource_barrier(cmd, actual_vp, ResourceState::UnorderedAccess, ResourceState::TransferDst);
				rhi->cmd_fill_buffer(cmd, actual_vp, 0, sizeof(uint32_t), 0);
				rhi->resource_barrier(cmd, actual_vp, ResourceState::TransferDst, ResourceState::UnorderedAccess);

				if (cascade_index == 1 || (cascade_index == 0 && config.cascade_count == 0)) {
					if (frame.page_request_buffer.is_valid()) {
						rhi->resource_barrier(cmd, frame.page_request_buffer, ResourceState::UnorderedAccess, ResourceState::TransferDst);
						// Clear 3 header uints: request_count, overflow_count, error_flags
						rhi->cmd_fill_buffer(cmd, frame.page_request_buffer, 0, 3 * sizeof(uint32_t), 0);
						rhi->resource_barrier(cmd, frame.page_request_buffer, ResourceState::TransferDst, ResourceState::UnorderedAccess);
					}
				}

				if (frame.visible_clusters.is_valid()) {
					rhi->resource_barrier(cmd, frame.visible_clusters, ResourceState::UnorderedAccess, ResourceState::TransferDst);
					rhi->cmd_fill_buffer(cmd, frame.visible_clusters, 0, sizeof(uint32_t), 0);
					rhi->resource_barrier(cmd, frame.visible_clusters, ResourceState::TransferDst, ResourceState::UnorderedAccess);
				}

				if (cascade_index == 0) {
					if (frame.indirect_draw.is_valid()) {
						// It could be in IndirectArgument state from the end of the previous frame.
						rhi->resource_barrier(cmd, frame.indirect_draw, ResourceState::IndirectArgument, ResourceState::TransferDst);
						rhi->cmd_fill_buffer(cmd, frame.indirect_draw, 0, sizeof(uint32_t), 0);
						rhi->resource_barrier(cmd, frame.indirect_draw, ResourceState::TransferDst, ResourceState::UnorderedAccess);
					}
				}

				rhi->cmd_bind_pipeline(cmd, hierarchy_traversal_pipeline);
				
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_compute_ubo(cmd, hierarchy_traversal_pipeline, 0);
				rhi->cmd_bind_storage_buffer(cmd, hierarchy_traversal_pipeline, 1, gpu_scene.get_page_table_buffer());
				// CSM cascade traversals get a FULL-SCENE instance list (source_instances)
				// so that casters outside the primary camera frustum are still rasterized
				// into the shadow map; the main-view traversal keeps the visible subset.
				BufferHandle traversal_instances =
					source_instances.is_valid() ? source_instances : frame.instance_data;
				rhi->cmd_bind_storage_buffer(cmd, hierarchy_traversal_pipeline, 2, traversal_instances);
				rhi->cmd_bind_storage_buffer(cmd, hierarchy_traversal_pipeline, 3, gpu_scene.get_vg_pool().group_buffer); 

				// binding 4 = PageRequestBuffer, binding 5 = VisiblePageBuffer
				if (frame.page_request_buffer.is_valid()) {
					rhi->cmd_bind_storage_buffer(cmd, hierarchy_traversal_pipeline, 4, frame.page_request_buffer);
				}
				rhi->cmd_bind_storage_buffer(cmd, hierarchy_traversal_pipeline, 5, actual_vp);

				struct Push {
					uint32_t instance_count;
					uint32_t enable_frustum_cull;
					uint32_t enable_lod;
					float screen_height;
					uint32_t max_requests;
					uint32_t cascade_index;
					float lod_error_scale;
					float ortho_extent;
				} push;
				push.instance_count = static_cast<uint32_t>(instance_count);
				push.enable_frustum_cull = 1u;
				push.enable_lod = config.enable_virtual_geometry ? 1u : 0u;
				push.screen_height = (cascade_index == 0) ? view.viewport_height : static_cast<float>(config.shadow_map_size);
				push.max_requests = 4093;
				push.cascade_index = cascade_index;
				push.lod_error_scale = lod_error_scale;
				push.ortho_extent = ortho_extent;
				rhi->cmd_push_constants(cmd, hierarchy_traversal_pipeline, sizeof(Push), &push);

				rhi->cmd_dispatch(cmd, (push.instance_count + 63) / 64, 1, 1);

				// Copy page requests to host-visible readback buffer for main view streaming
				if (cascade_index == 0) {
					if (frame.page_request_readback.is_valid()) {
						rhi->resource_barrier(cmd, frame.page_request_buffer,
							ResourceState::UnorderedAccess, ResourceState::TransferSrc);
						rhi->resource_barrier(cmd, frame.page_request_readback,
							ResourceState::UnorderedAccess, ResourceState::TransferDst);
						rhi->cmd_copy_buffer(cmd, frame.page_request_buffer, frame.page_request_readback, 16384);
						rhi->resource_barrier(cmd, frame.page_request_buffer,
							ResourceState::TransferSrc, ResourceState::UnorderedAccess);
						rhi->resource_barrier(cmd, frame.page_request_readback,
							ResourceState::TransferDst, ResourceState::UnorderedAccess);
					}

					if (frame.visible_pages_readback.is_valid()) {
						rhi->resource_barrier(cmd, frame.visible_pages,
							ResourceState::UnorderedAccess, ResourceState::TransferSrc);
						rhi->resource_barrier(cmd, frame.visible_pages_readback,
							ResourceState::UnorderedAccess, ResourceState::TransferDst);
						rhi->cmd_copy_buffer(cmd, frame.visible_pages, frame.visible_pages_readback, 4 + 4096 * sizeof(uint32_t));
						rhi->resource_barrier(cmd, frame.visible_pages,
							ResourceState::TransferSrc, ResourceState::UnorderedAccess);
						rhi->resource_barrier(cmd, frame.visible_pages_readback,
							ResourceState::TransferDst, ResourceState::UnorderedAccess);
					}
				}
			}
		);
	}

	// ---------------------------------------------------------
	// PageEmitPass
	// ---------------------------------------------------------
	PageEmitPass::~PageEmitPass() = default;

	void PageEmitPass::shutdown(RHI* rhi) {
		if (page_emit_pipeline.is_valid()) {
			rhi->destroy_pipeline(page_emit_pipeline);
			page_emit_pipeline.reset();
		}
	}

	void PageEmitPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		load_shaders_async(asset_manager, { "src/shaders/page_emit.comp.spv" }, [this, rhi](std::vector<std::vector<char>> shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::PageEmit;
			desc.cs.code = shaders[0];
			page_emit_pipeline = rhi->create_compute_pipeline(desc);
			if (page_emit_pipeline.is_valid()) bud::print("[PageEmitPass] Shader loaded and pipeline created.");
		});
	}

	void PageEmitPass::add_to_graph(RenderGraph& rg, const RenderConfig& config, const GPUScene& gpu_scene, uint32_t current_frame) {
		if (!page_emit_pipeline.is_valid()) return;

		const auto& frame = gpu_scene.get_frame_resources(current_frame);

		if (!frame.visible_pages.is_valid() || !frame.visible_clusters.is_valid())
			return;

		RGHandle rg_visible_pages = rg.import_buffer("VisiblePages", frame.visible_pages, ResourceState::UnorderedAccess);
		RGHandle rg_visible_clusters = rg.import_buffer("VisibleClusters", frame.visible_clusters, ResourceState::UnorderedAccess);

		rg.add_pass("Page Emit",
			[=](RGBuilder& builder) {
				builder.read(rg_visible_pages, ResourceState::ShaderResource);
				builder.write(rg_visible_clusters, ResourceState::UnorderedAccess);
			},
			[=, &rg, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				const auto& frame = gpu_scene.get_frame_resources(current_frame);
				rhi->cmd_bind_pipeline(cmd, page_emit_pipeline);
				
				rhi->cmd_bind_storage_buffer(cmd, page_emit_pipeline, 0, gpu_scene.get_page_pool_buffer());
				rhi->cmd_bind_storage_buffer(cmd, page_emit_pipeline, 1, frame.visible_pages);
				rhi->cmd_bind_storage_buffer(cmd, page_emit_pipeline, 2, frame.visible_clusters);
				// Binding 3 (page_table) is not needed by page_emit.comp, but harmless if it was bound.

				uint32_t max_clusters = frame.visible_cluster_capacity;
				rhi->cmd_push_constants(cmd, page_emit_pipeline, sizeof(uint32_t), &max_clusters);

				// page_emit.comp uses local_size_x = 64
				rhi->cmd_dispatch(cmd, (frame.visible_page_capacity + 63) / 64, 1, 1);
			}
		);
	}

	// ---------------------------------------------------------
	// ClusterCullPass
	// ---------------------------------------------------------
	ClusterCullPass::~ClusterCullPass() = default;

	void ClusterCullPass::shutdown(RHI* rhi) {
		if (cluster_cull_pipeline.is_valid()) {
			rhi->destroy_pipeline(cluster_cull_pipeline);
			cluster_cull_pipeline.reset();
		}
		if (clear_stats_pipeline.is_valid()) {
			rhi->destroy_pipeline(clear_stats_pipeline);
			clear_stats_pipeline.reset();
		}
	}

	void ClusterCullPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		load_shaders_async(asset_manager, {
			"src/shaders/clear_stats.comp.spv",
			"src/shaders/cluster_cull.comp.spv"
		}, [this, rhi](std::vector<std::vector<char>> shaders) {
			// Clear stats pipeline
			ComputePipelineDesc clear_desc;
			clear_desc.layout_kind = ComputePipelineDesc::LayoutKind::ClearStats;
			clear_desc.cs.code = shaders[0];
			clear_stats_pipeline = rhi->create_compute_pipeline(clear_desc);
			if (clear_stats_pipeline.is_valid()) bud::print("[ClusterCullPass] ClearStats shader loaded and pipeline created.");

			// Cluster cull pipeline
			ComputePipelineDesc cull_desc;
			cull_desc.layout_kind = ComputePipelineDesc::LayoutKind::ClusterCull;
			cull_desc.cs.code = shaders[1];
			cluster_cull_pipeline = rhi->create_compute_pipeline(cull_desc);
			if (cluster_cull_pipeline.is_valid()) bud::print("[ClusterCullPass] Shader loaded and pipeline created.");
		});
	}

	void ClusterCullPass::add_to_graph(RenderGraph& rg, RGHandle hiz_pyramid, RGHandle rg_draw, const SceneView& view, const RenderConfig& config, const GPUScene& gpu_scene, uint32_t current_frame) {
		if (!cluster_cull_pipeline.is_valid()) return;

		const auto& frame = gpu_scene.get_frame_resources(current_frame);

		if (!frame.visible_clusters.is_valid() || !frame.indirect_draw.is_valid())
			return;

		RGHandle rg_visible_clusters = rg.import_buffer("VisibleClusters", frame.visible_clusters, ResourceState::UnorderedAccess);
		RGHandle rg_dynamic_instances = rg.import_buffer("DynamicInstances", frame.dynamic_instances, ResourceState::UnorderedAccess);

		rg.add_pass("Cluster Cull",
			[=](RGBuilder& builder) {
				builder.read(rg_visible_clusters, ResourceState::ShaderResource);
				if (hiz_pyramid.is_valid()) {
					builder.read(hiz_pyramid, ResourceState::ShaderResource);
				}
				builder.write(rg_draw, ResourceState::UnorderedAccess);
				builder.write(rg_dynamic_instances, ResourceState::UnorderedAccess);
			},
			[=, &rg, &view, &config, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				const auto& frame = gpu_scene.get_frame_resources(current_frame);

				// Clear stats buffer using compute shader (avoids transfer barrier).
				if (frame.stats_readback.is_valid() && clear_stats_pipeline.is_valid()) {
					rhi->cmd_bind_pipeline(cmd, clear_stats_pipeline);
					rhi->cmd_bind_storage_buffer(cmd, clear_stats_pipeline, 0, frame.stats_readback);
					uint32_t dword_count = sizeof(bud::graphics::GPUStats) / sizeof(uint32_t);
					rhi->cmd_push_constants(cmd, clear_stats_pipeline, sizeof(uint32_t), &dword_count);
					rhi->cmd_dispatch(cmd, (dword_count + 63) / 64, 1, 1);
				}

				rhi->resource_barrier(cmd, frame.visible_clusters, ResourceState::UnorderedAccess, ResourceState::ShaderResource);

rhi->cmd_bind_pipeline(cmd, cluster_cull_pipeline);

rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 0, frame.instance_data);
				rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 1, gpu_scene.get_page_table_buffer());
				rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 2, gpu_scene.get_page_pool_buffer());
				rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 3, frame.visible_clusters);
				rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 4, frame.indirect_draw);
				rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 5, frame.dynamic_instances);
				rhi->cmd_bind_storage_buffer(cmd, cluster_cull_pipeline, 6, frame.stats_readback);
				
				TextureHandle hiz_tex;
				if (hiz_pyramid.is_valid()) {
					try { hiz_tex = rg.get_texture(hiz_pyramid); } catch (...) {}
				}
				if (!hiz_tex.is_valid()) {
					hiz_tex = rhi->get_fallback_texture();
				}
				rhi->cmd_bind_compute_texture(cmd, cluster_cull_pipeline, 7, hiz_tex);

				rhi->cmd_bind_compute_ubo(cmd, cluster_cull_pipeline, 8); // ubo bound at 8, from common.glsl

				struct ClusterCullPush {
					uint32_t max_clusters;
					float screen_height;
					float error_threshold;
				} push;
				push.max_clusters = frame.indirect_capacity;
				push.screen_height = view.viewport_height;
				push.error_threshold = config.lod_error_threshold_px;
				rhi->cmd_push_constants(cmd, cluster_cull_pipeline, sizeof(ClusterCullPush), &push);

				// Dispatch enough to cover maximum visible clusters (which is visible_cluster_capacity)
				rhi->cmd_dispatch(cmd, (frame.visible_cluster_capacity + 255) / 256, 1, 1);
			}
		);
	}

}
