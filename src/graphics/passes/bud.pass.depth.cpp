#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"
#include "src/graphics/bud.graphics.scene.hpp"
#include "src/io/bud.io.hpp"

#include <cmath>
#include <format>
#include <stdexcept>
#include <algorithm>

namespace bud::graphics {

	void DepthOnlyPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("DepthOnlyPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/depth_only.vert.spv", "src/shaders/depth_only.frag.spv" }, [this, rhi, config](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = true;
			desc.depth_write = true;
			desc.cull_mode = CullMode::None;
			desc.color_attachment_format = bud::graphics::TextureFormat::Undefined;
			desc.depth_compare_op = config.reversed_z ? CompareOp::Greater : CompareOp::Less;
			desc.enable_depth_bias = false;
			desc.vertex_layout = VertexLayoutType::PositionUV;

			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline.is_valid()) {
				bud::print("[DepthOnlyPass] Shaders loaded and pipeline created.");
			}
			});
	}

	RGHandle DepthOnlyPass::add_to_graph(RenderGraph& render_graph, RGHandle backbuffer,
		const RenderScene& render_scene,
		const SceneView& view,
		const RenderConfig& config,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,
		size_t instance_count,
		RGHandle indirect_draw_buffer,
		const GPUScene& gpu_scene,
		bud::graphics::BufferHandle mega_vertex_buffer,
		bud::graphics::BufferHandle mega_index_buffer,
		RGHandle existing_depth_buffer,
		size_t split_index) {
		if (!pipeline.is_valid()) {
			std::string err = "DepthOnlyPass::add_to_graph pipeline is invalid";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return {};
#endif
		}

		const size_t max_scene_count = std::min({
			render_scene.world_matrices.size(),
			render_scene.world_aabbs.size(),
			render_scene.mesh_indices.size(),
			render_scene.material_indices.size(),
			render_scene.flags.size()
			});

		const bool use_indirect_draw = indirect_draw_buffer.is_valid();
		if (max_scene_count == 0 || (!use_indirect_draw && sort_list.empty())) {
			std::string err = "ZPrepass::add_to_graph empty scene or sort list";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return {};
#endif
		}

		auto backbuffer_desc = render_graph.get_texture_desc(backbuffer);
		if (backbuffer_desc.width == 0 || backbuffer_desc.height == 0) {
			bud::eprint("ZPrepass::add_to_graph invalid backbuffer: w={} h={}", backbuffer_desc.width, backbuffer_desc.height);
#if defined(_DEBUG)
			throw std::runtime_error("ZPrepass::add_to_graph invalid backbuffer");
#else
			return {};
#endif
		}

		const size_t draw_count = std::min(instance_count, sort_list.size());
		uint32_t target_width = backbuffer_desc.width;
		uint32_t target_height = backbuffer_desc.height;

		TextureDesc depth_desc;
		depth_desc.width = target_width;
		depth_desc.height = target_height;
		depth_desc.format = bud::graphics::TextureFormat::D32_FLOAT;

		auto depth_h = std::make_shared<RGHandle>(existing_depth_buffer);
		bool clear_depth_flag = !existing_depth_buffer.is_valid();
        
		return render_graph.add_pass(clear_depth_flag ? "Depth Only Pass" : "Depth Only Pass (Phase 2)",
			[=](RGBuilder& builder) {
				if (clear_depth_flag) {
					*depth_h = builder.create("MainDepth", depth_desc);
				}
				*depth_h = builder.write(*depth_h, ResourceState::DepthWrite);
				if (use_indirect_draw && indirect_draw_buffer.is_valid()) {
					builder.read(indirect_draw_buffer, ResourceState::IndirectArgument);
				}
				return *depth_h;
			},
			[=, &render_graph, &render_scene, &meshes, &sort_list, &gpu_scene, this](RHI* rhi, CommandHandle cmd) {
				if (!pipeline.is_valid()) {
					bud::eprint("[DepthOnlyPass] ERROR: Pipeline is invalid.");
					return;
				}

				RenderPassBeginInfo info;
				info.depth_attachment = render_graph.get_texture(*depth_h);
				info.clear_depth = clear_depth_flag;
				info.clear_depth_value = config.reversed_z ? 0.0f : 1.0f;

				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->cmd_set_viewport(cmd, (float)target_width, (float)target_height);
				rhi->cmd_set_scissor(cmd, target_width, target_height);

				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);

				// Bind global Mega-Buffer once for the entire pass
				rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
				rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);

				const auto page_pool_buf = gpu_scene.get_page_pool_buffer();

				if (use_indirect_draw) {
					bud::graphics::BufferHandle indirect_buffer_handle;
					try {
						indirect_buffer_handle = render_graph.get_buffer(indirect_draw_buffer);
					}
					catch (const std::exception& e) {
						bud::eprint("[DepthOnlyPass] failed to get indirect draw buffer: {}", e.what());
						return;
					}

					if (!indirect_buffer_handle.is_valid()) {
						bud::eprint("[DepthOnlyPass] invalid indirect draw buffer.");
						return;
					}

					if (indirect_draw_buffer.is_valid() && draw_count > 0) {
						auto page_pool_buf = gpu_scene.get_page_pool_buffer();

						uint32_t gpu_draw_count = static_cast<uint32_t>(draw_count);
						if (config.enable_virtual_geometry) {
							uint32_t frame_idx = rhi->get_current_frame_index();
							gpu_draw_count = std::max(gpu_draw_count,
								gpu_scene.get_frame_resources(frame_idx).indirect_capacity);
						}

						if (split_index > 0) {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
							rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), 0, static_cast<uint32_t>(split_index), sizeof(bud::graphics::IndirectCommand));
						}

						if (split_index < gpu_draw_count && page_pool_buf.is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, page_pool_buf);
							rhi->cmd_bind_index_buffer(cmd, page_pool_buf, true);
							rhi->cmd_draw_indexed_indirect(cmd, render_graph.get_buffer(indirect_draw_buffer), split_index * sizeof(bud::graphics::IndirectCommand), static_cast<uint32_t>(gpu_draw_count - split_index), sizeof(bud::graphics::IndirectCommand));
						}
					}
				}
				else {
					for (size_t i = 0; i < draw_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t idx = item.entity_index;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						uint32_t material_id = render_scene.material_indices[idx];
						const auto& model_matrix = render_scene.world_matrices[idx];

						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_id);

						if (mesh.is_page_based && gpu_scene.get_page_pool_buffer().is_valid()) {
							rhi->cmd_bind_vertex_buffer(cmd, gpu_scene.get_page_pool_buffer());
							rhi->cmd_bind_index_buffer(cmd, gpu_scene.get_page_pool_buffer(), true);
						}
						else {
							rhi->cmd_bind_vertex_buffer(cmd, mega_vertex_buffer);
							rhi->cmd_bind_index_buffer(cmd, mega_index_buffer);
						}

						uint32_t vtx_off = (mesh.is_page_based && gpu_scene.get_page_pool_buffer().is_valid()) ? 0 : mesh_geometry.vertex_offset;
						if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[item.submesh_index];
							rhi->cmd_draw_indexed(cmd, sub.index_count, 1, mesh_geometry.first_index + sub.index_start, vtx_off, (uint32_t)i);
						}
						else {
							rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, mesh_geometry.first_index, vtx_off, (uint32_t)i);
						}
					}
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}

	void PyramidMipPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("PyramidMipPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/hiz_mip.comp.spv" }, [this, rhi](const auto& shaders) {
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::HiZMip;
			desc.cs.code = shaders[0];
			pipeline = rhi->create_compute_pipeline(desc);
			if (pipeline.is_valid()) {
				bud::print("[PyramidMipPass] Shader loaded and pipeline created.");
			}
			});
	}

	RGHandle PyramidMipPass::add_to_graph(RenderGraph& rg, RGHandle depth_buffer, const RenderConfig& config, RGHandle target_pyramid) {
		if (!pipeline.is_valid()) return {};

		auto depth_desc = rg.get_texture_desc(depth_buffer);
		if (depth_desc.width == 0 || depth_desc.height == 0) return {};

		// Create a POT pyramid texture for easy mip generation
		uint32_t pot_w = 1 << (uint32_t)std::ceil(std::log2((float)depth_desc.width));
		uint32_t pot_h = 1 << (uint32_t)std::ceil(std::log2((float)depth_desc.height));
		uint32_t size = std::max(pot_w, pot_h);
		uint32_t mip_count = (uint32_t)std::floor(std::log2((float)size)) + 1;

		TextureDesc desc;
		desc.width = size;
		desc.height = size;
		desc.mips = mip_count;
		desc.format = bud::graphics::TextureFormat::R32_FLOAT; // Standard format for HiZ
		desc.is_storage = true;
		desc.initial_state = bud::graphics::ResourceState::Undefined;

		auto pyramid_h_ptr = std::make_shared<RGHandle>(target_pyramid);

		for (uint32_t i = 0; i < mip_count; ++i) {
			rg.add_pass(std::format("Hi-Z Mip {}", i),
				[=](RGBuilder& builder) {
					builder.set_queue(QueueType::AsyncCompute);
					if (i == 0 && !pyramid_h_ptr->is_valid()) {
						*pyramid_h_ptr = builder.create("HiZPyramid", desc);
					}
					RGHandle current_pyramid = *pyramid_h_ptr;
					RGHandle src_handle = (i == 0) ? depth_buffer : current_pyramid;
					ResourceState src_read_state = (i == 0) ? ResourceState::ShaderResource : ResourceState::UnorderedAccess;

					builder.read(src_handle, src_read_state);
					builder.write(current_pyramid, ResourceState::UnorderedAccess);
					return current_pyramid;
				},
				[=, &rg](RHI* rhi, CommandHandle cmd) {
					RGHandle current_pyramid = *pyramid_h_ptr;
					RGHandle src_handle = (i == 0) ? depth_buffer : current_pyramid;

					uint32_t dst_size = size >> i;
					rhi->cmd_bind_pipeline(cmd, pipeline);
					TextureHandle src_tex{};
					TextureHandle dst_tex{};
					try {
						src_tex = rg.get_texture(src_handle);
						dst_tex = rg.get_texture(current_pyramid);
					}
					catch (const std::exception& e) {
						bud::eprint("[PyramidMipPass] Resource lookup failed: {}", e.what());
						return;
					}
					rhi->cmd_bind_compute_texture(cmd, pipeline, 3, src_tex, (i == 0) ? 0 : (i - 1), false, (i > 0)); // is_general=true when reading from the pyramid (it's in GENERAL layout)
					rhi->cmd_bind_compute_texture(cmd, pipeline, 5, dst_tex, i, true);

					struct Push {
						bud::math::vec2 out_size;
						uint32_t reversed_z;
						uint32_t padding;
					} push;
					push.out_size = bud::math::vec2((float)dst_size, (float)dst_size);
					push.reversed_z = config.reversed_z ? 1 : 0;
					rhi->cmd_push_constants(cmd, pipeline, sizeof(push), &push);

					uint32_t gx = (dst_size + 15) / 16;
					uint32_t gy = (dst_size + 15) / 16;
					rhi->cmd_dispatch(cmd, gx, gy, 1);
				}
			);
		}

		// Transition all mips of the completed Hi-Z pyramid to ShaderResource (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
		// so subsequent passes (CSM shadow next frame, cluster culling, visibility pass) can sample it safely.
		rg.add_pass("Hi-Z Final Transition",
			[=](RGBuilder& builder) {
				builder.set_queue(QueueType::AsyncCompute);
				builder.set_side_effect();
				builder.read(*pyramid_h_ptr, ResourceState::ShaderResource);
				return *pyramid_h_ptr;
			},
			[](RHI*, CommandHandle) {}
		);

		return *pyramid_h_ptr;
	}

	void PyramidMipDebugPass::init(RHI* rhi, const RenderConfig& config, bud::io::AssetManager* asset_manager) {
		if (!rhi || !asset_manager) {
			std::string err = std::format("PyramidMipDebugPass::init invalid args: rhi={} asset_manager={}", (void*)rhi, (void*)asset_manager);
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		load_shaders_async(asset_manager, { "src/shaders/fullscreen.vert.spv", "src/shaders/hiz_debug.frag.spv" }, [this, rhi](const auto& shaders) {
			GraphicsPipelineDesc desc;
			desc.vs.code = shaders[0];
			desc.fs.code = shaders[1];
			desc.depth_test = false;
			desc.depth_write = false;
			desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB;
			desc.vertex_layout = VertexLayoutType::NoVertexInput;
			pipeline = rhi->create_graphics_pipeline(desc);
			if (pipeline.is_valid()) {
				bud::print("[PyramidMipDebugPass] Shaders loaded and pipeline created.");
			}
			});
	}

	void PyramidMipDebugPass::add_to_graph(RenderGraph& rg, RGHandle backbuffer, RGHandle hiz_pyramid, uint32_t mip_level) {
		if (!pipeline.is_valid()) return;
		rg.add_pass("Hi-Z Debug Pass",
			[=](RGBuilder& builder) {
				builder.read(hiz_pyramid, ResourceState::ShaderResource);
				builder.write(backbuffer, ResourceState::RenderTarget);
			},
			[=, &rg](RHI* rhi, CommandHandle cmd) {
				rhi->cmd_begin_debug_label(cmd, "Hi-Z Debug", 1, 0, 1);
				rhi->cmd_bind_pipeline(cmd, pipeline);
				rhi->update_bindless_image(0, rg.get_texture(hiz_pyramid), ALL_MIPS, false);

				struct PC { uint32_t mip; } pc = { mip_level };
				rhi->cmd_push_constants(cmd, pipeline, sizeof(pc), &pc);

				RenderPassBeginInfo info;
				info.color_attachments.push_back(rg.get_texture(backbuffer));
				rhi->cmd_begin_render_pass(cmd, info);
				rhi->cmd_draw(cmd, 3, 1, 0, 0);
				rhi->cmd_end_render_pass(cmd);
				rhi->cmd_end_debug_label(cmd);
			}
		);
	}

}
