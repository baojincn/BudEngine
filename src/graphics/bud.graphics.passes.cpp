#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <cmath>

#include "src/graphics/bud.graphics.passes.hpp"

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"

#include "src/graphics/bud.graphics.scene.hpp"

namespace bud::graphics {

    // --- CSMShadowPass ---

    void CSMShadowPass::init(RHI* rhi) {
        stored_rhi = rhi;
        auto vs_code = bud::io::FileSystem::read_binary("src/shaders/shadow.vert.spv");
        // Shadow pass theoretically doesn't need frag shader for D32, but we have one
        auto fs_code = bud::io::FileSystem::read_binary("src/shaders/shadow.frag.spv"); 

        if (!vs_code) throw std::runtime_error("[CSMShadowPass] Failed to load shadow.vert.spv");
        if (!fs_code) throw std::runtime_error("[CSMShadowPass] Failed to load shadow.frag.spv");

        if (!vs_code || !fs_code) {
            return;
        }

        GraphicsPipelineDesc desc;
        desc.vs.code = *vs_code;
        desc.fs.code = *fs_code;
        desc.cull_mode = CullMode::Front; // Only render backfaces
        desc.color_attachment_format = TextureFormat::Undefined;

        
        pipeline = rhi->create_graphics_pipeline(desc);
        if (pipeline) {
            std::println("[CSMShadowPass] Pipeline created successfully: {}", (void*)pipeline);
        } else {
            std::println(stderr, "[CSMShadowPass] ERROR: Pipeline creation returned nullptr!");
        }
    }

	// bud.graphics.passes.cpp

	RGHandle CSMShadowPass::add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config,
		const RenderScene& render_scene, // [1] 改为 RenderScene
		const std::vector<RenderMesh>& meshes)
	{
		// ... 纹理描述 desc 创建 (保持不变) ...
		TextureDesc desc;
		desc.width = config.shadow_map_size;
		desc.height = config.shadow_map_size;
		desc.format = TextureFormat::D32_FLOAT;
		desc.type = TextureType::Texture2DArray;
		desc.array_layers = config.cascade_count;

		// ... 缓存检查逻辑 (保持不变) ...
		bool light_changed = bud::math::length(view.light_dir - last_light_dir) > 0.001f;
		bool need_update = !cache_initialized || light_changed || !config.cache_shadows;

		if (config.cache_shadows) {
			if (light_changed) last_light_dir = view.light_dir;
			if (stored_rhi && (!static_cache_texture || static_cache_texture->width != desc.width)) {
				static_cache_texture = stored_rhi->create_texture(desc, nullptr, 0);
				need_update = true;
			}
		}

		auto shadow_map_h = std::make_shared<RGHandle>();

		// [CSM] 2. Static Cache Update Pass
		RGHandle static_cache_h;
		bool valid_cache = config.cache_shadows && static_cache_texture;

		if (valid_cache) {
			static_cache_h = rg.import_texture("StaticShadowCache", static_cache_texture, ResourceState::ShaderResource);

			if (need_update) {
				rg.add_pass("CSM Static Update",
					[&](RGBuilder& builder) {
						builder.write(static_cache_h, ResourceState::DepthWrite);
						return static_cache_h;
					},
					// [关键] Lambda 捕获 render_scene
					[=, &rg, &render_scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
						if (!pipeline) return;

						for (uint32_t i = 0; i < config.cascade_count; ++i) {
							// ... Viewport, Scissor, Bind Pipeline (保持不变) ...
							auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
							bud::math::Frustum cascade_view_frustum_dbg;
							cascade_view_frustum_dbg.update(cascade_light_view_proj);

							// Setup RenderPass ...
							RenderPassBeginInfo info;
							info.depth_attachment = rg.get_texture(static_cache_h);
							info.clear_depth = true;
							info.base_array_layer = i;
							info.layer_count = 1;
							rhi->cmd_begin_render_pass(cmd, info);

							rhi->cmd_bind_pipeline(cmd, pipeline);
							rhi->cmd_set_viewport(cmd, (float)config.shadow_map_size, (float)config.shadow_map_size);
							rhi->cmd_set_scissor(cmd, config.shadow_map_size, config.shadow_map_size);

							// PushConsts Setup (light info)...
							struct PushConsts {
								bud::math::mat4 light_view_proj;
								bud::math::mat4 model;
								bud::math::vec4 light_dir;
								uint32_t material_id;
							} push_consts;

							push_consts.light_view_proj = cascade_light_view_proj;
							push_consts.light_dir = bud::math::vec4(bud::math::normalize(view.light_dir), 0.0f);

							// [核心修改] 遍历 RenderScene
							size_t count = render_scene.instance_count.load(std::memory_order_relaxed);
							for (size_t idx = 0; idx < count; ++idx) {

								// 1. 检查是否是静态物体 (利用我们刚加的 flags)
								auto is_static = (render_scene.flags[idx] & 1) != 0;
								if (!is_static) continue; // ONLY STATIC for cache

								auto mesh_id = render_scene.mesh_indices[idx];

								if (mesh_id >= meshes.size()) continue;
								const auto& mesh = meshes[mesh_id];
								if (!mesh.is_valid()) continue;

								auto mat_id = render_scene.material_indices[idx];
								push_consts.material_id = mat_id;

								// 2. Culling
								// 使用 RenderScene 里的 World Matrix 变换包围体
								const auto& model_matrix = render_scene.world_matrices[idx];
								bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
								if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;

								// 使用 RenderScene 里预计算好的 World AABB (更快！)
								const auto& world_aabb = render_scene.world_aabbs[idx];
								if (!bud::math::intersect_aabb_frustum(world_aabb, cascade_view_frustum_dbg)) continue;

								// 3. Draw
								push_consts.model = model_matrix;
								rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);
								rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
								rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);
								rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
							}
							rhi->cmd_end_render_pass(cmd);
						}
					}
				);
				cache_initialized = true;
			}
		}

		// [CSM] 3. Main Shadow Pass (Dynamic + Copy)
		return rg.add_pass("CSM Shadow",
			[&, shadow_map_h](RGBuilder& builder) {
				*shadow_map_h = builder.create("CSM ShadowMap", desc);
				builder.write(*shadow_map_h, ResourceState::DepthWrite);
				if (valid_cache) builder.read(static_cache_h);
				return *shadow_map_h;
			},
			[=, &rg, &render_scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
				if (!pipeline) return;

				Texture* active_map = rg.get_texture(*shadow_map_h);
				bool did_copy = false;

				// ... Copy Logic (保持不变) ...
				if (valid_cache && cache_initialized) {
					Texture* static_map = rg.get_texture(static_cache_h);
					rhi->resource_barrier(cmd, static_map, ResourceState::ShaderResource, ResourceState::TransferSrc);
					rhi->resource_barrier(cmd, active_map, ResourceState::DepthWrite, ResourceState::TransferDst);
					rhi->cmd_copy_image(cmd, static_map, active_map);
					rhi->resource_barrier(cmd, active_map, ResourceState::TransferDst, ResourceState::DepthWrite);
					rhi->resource_barrier(cmd, static_map, ResourceState::TransferSrc, ResourceState::ShaderResource);
					did_copy = true;
				}

				for (uint32_t i = 0; i < config.cascade_count; ++i) {
					// ... Viewport, Pass Begin (保持不变) ...
					auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
					bud::math::Frustum cascade_view_frustum_dbg;
					cascade_view_frustum_dbg.update(cascade_light_view_proj);

					RenderPassBeginInfo info;
					info.depth_attachment = active_map;
					info.clear_depth = !did_copy; // 如果 Copy 了就不用 Clear
					info.base_array_layer = i;
					info.layer_count = 1;

					rhi->cmd_begin_render_pass(cmd, info);
					rhi->cmd_bind_pipeline(cmd, pipeline);
					// ... Push Constants & Depth Bias Setup (保持不变) ...

					// Push Constant Setup
					struct PushConsts {
						bud::math::mat4 light_view_proj;
						bud::math::mat4 model;
						bud::math::vec4 light_dir;
						uint32_t material_id;
					} push_consts;
					push_consts.light_view_proj = cascade_light_view_proj;
					push_consts.light_dir = bud::math::vec4(bud::math::normalize(view.light_dir), 0.0f);

					// [核心修改] 遍历 RenderScene
					size_t count = render_scene.instance_count.load(std::memory_order_relaxed);
					for (size_t idx = 0; idx < count; ++idx) {

						// 如果我们已经 Copy 了静态缓存，这里就只画动态物体
						bool is_static = (render_scene.flags[idx] & 1) != 0;
						if (did_copy && is_static) continue;

						uint32_t mesh_id = render_scene.mesh_indices[idx];
						if (mesh_id >= meshes.size()) continue;
						const auto& mesh = meshes[mesh_id];
						if (!mesh.is_valid()) continue;

						auto mat_id = render_scene.material_indices[idx];
						push_consts.material_id = mat_id;

						// Culling
						const auto& model_matrix = render_scene.world_matrices[idx];
						bud::math::BoundingSphere world_sphere = mesh.sphere.transform(model_matrix);
						if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;

						const auto& world_aabb = render_scene.world_aabbs[idx];
						if (!bud::math::intersect_aabb_frustum(world_aabb, cascade_view_frustum_dbg)) continue;

						// Draw
						push_consts.model = model_matrix;
						rhi->cmd_push_constants(cmd, pipeline, sizeof(PushConsts), &push_consts);

						rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
						rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);
						rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
					}
					rhi->cmd_end_render_pass(cmd);
				}
			}
		);
	}
	

    // --- MainPass ---

    void MainPass::init(RHI* rhi) {
        auto vs_code = bud::io::FileSystem::read_binary("src/shaders/main.vert.spv");
        auto fs_code = bud::io::FileSystem::read_binary("src/shaders/main.frag.spv");

        if (!vs_code || !fs_code) {
            std::println(stderr, "[MainPass] Failed to load shaders!");
            return;
        }

        GraphicsPipelineDesc desc;
        desc.vs.code = *vs_code;
        desc.fs.code = *fs_code;
        desc.depth_test = true;
        desc.depth_write = true;
        desc.cull_mode = CullMode::None; // [FIX] Sponza Cloth (Double Sided)
        desc.color_attachment_format = bud::graphics::TextureFormat::BGRA8_SRGB; // [FIX] Match Swapchain (SRGB) to fix Validation Error
        
        // Use default Vertex Layout (Position, Color, Normal, UV, Index)
        // Implemented in RHI hardcoded for now or use desc.vertex_layout
        // Note: My RHI implementation ignored desc.vertex_layout and hardcoded it.
        // This is fine for this sample.

        pipeline = rhi->create_graphics_pipeline(desc);
        std::println("[MainPass] Pipeline created successfully.");
    }

	void MainPass::add_to_graph(RenderGraph& rg, RGHandle shadow_map, RGHandle backbuffer,
		const RenderScene& render_scene,       // [1] 改为 RenderScene
		const SceneView& view,
		const std::vector<RenderMesh>& meshes,
		const std::vector<SortItem>& sort_list,// [2] 新增参数
		size_t instance_count)                 // [3] 新增参数
	{
		TextureDesc depth_desc;
		depth_desc.width = (uint32_t)view.viewport_width;
		depth_desc.height = (uint32_t)view.viewport_height;
		depth_desc.format = TextureFormat::D32_FLOAT;

		auto depth_h = std::make_shared<RGHandle>();

		rg.add_pass("Main Lighting Pass",
			[=](RGBuilder& builder) {
				// ... 保持不变 ...
				builder.write(backbuffer, ResourceState::RenderTarget);
				builder.read(shadow_map, ResourceState::ShaderResource);
				*depth_h = builder.create("MainDepth", depth_desc);
				builder.write(*depth_h, ResourceState::DepthWrite);
				return *depth_h;
			},
			// [关键] Lambda 捕获要加上 render_scene, sort_list, instance_count
			// 注意：capture by reference (&) 小心悬垂引用，但在 RenderGraph 立即执行模式下通常没问题。
			// 建议按值捕获指针或引用包装，或者确保 renderer 生命周期长于 graph execute。
			[=, &rg, &render_scene, &meshes, &sort_list, this](RHI* rhi, CommandHandle cmd) {

				// ... setup render pass (begin_render_pass, viewport, scissor) ...
				// 代码省略，保持你原有的 setup ...

				RenderPassBeginInfo info;
				info.color_attachments.push_back(rg.get_texture(backbuffer));
				info.depth_attachment = rg.get_texture(*depth_h);
				info.clear_color = true;
				info.clear_color_value = { 0.5f, 0.5f, 0.5f, 1.0f };
				info.clear_depth = true;

				rhi->cmd_begin_render_pass(cmd, info);

				if (pipeline)
					rhi->cmd_bind_pipeline(cmd, pipeline);

				rhi->cmd_set_viewport(cmd, view.viewport_width, view.viewport_height);
				rhi->cmd_set_scissor(cmd, (uint32_t)view.viewport_width, (uint32_t)view.viewport_height);

				// Global Set Bindings
				rhi->update_global_shadow_map(rg.get_texture(shadow_map));
				rhi->update_global_uniforms(rhi->get_current_image_index(), view);
				if (pipeline)
					rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);


				// =========================================================
				// [核心修复] 使用 SortList 进行渲染，而不是遍历 Entities
				// =========================================================

				uint32_t last_material = -1;
				uint32_t last_mesh = -1;

				for (size_t i = 0; i < instance_count; ++i) {
					const auto& item = sort_list[i];
					uint32_t idx = item.entity_index; // 回查 RenderScene 的索引

					// 1. 从 RenderScene 获取扁平化数据 (SoA)
					// 此时不需要访问 entity 对象，数据都在 RenderScene 的数组里
					uint32_t mesh_id = render_scene.mesh_indices[idx];
					uint32_t mat_id = render_scene.material_indices[idx];
					const auto& model_matrix = render_scene.world_matrices[idx];

					// 安全检查
					if (mesh_id >= meshes.size()) continue;
					const auto& mesh = meshes[mesh_id];
					if (!mesh.is_valid()) continue;


					if (mesh_id != last_mesh) {
						rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
						rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);
						last_mesh = mesh_id;
					}

					// 3. Push Constants
					struct PushVars {
						bud::math::mat4 model;
						uint32_t material_id;  // [新增] 告诉 Shader 用哪个材质
						uint32_t padding[3];   // 保持 16 字节对齐 (可选，但推荐)
					} push_vars;

					push_vars.model = model_matrix; // 直接使用 RenderScene 的矩阵
					push_vars.material_id = mat_id;

					rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);

					// 4. Draw
					rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);
				}

				rhi->cmd_end_render_pass(cmd);
			}
		);
	}
}
