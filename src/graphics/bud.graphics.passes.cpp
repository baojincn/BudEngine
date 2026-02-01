module;
#include <vector>
#include <print>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stdexcept>
#include <cmath>

module bud.graphics.passes;

import bud.io;
import bud.graphics.rhi;
import bud.graphics.types;
import bud.graphics.graph;
import bud.scene;
import bud.math;

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

    RGHandle CSMShadowPass::add_to_graph(RenderGraph& rg, const SceneView& view, const RenderConfig& config, 
        const bud::scene::Scene& scene, const std::vector<RenderMesh>& meshes) 
    {
        TextureDesc desc;
        desc.width = config.shadow_map_size;
        desc.height = config.shadow_map_size;
        desc.format = TextureFormat::D32_FLOAT;
        desc.type = TextureType::Texture2DArray;
        desc.array_layers = config.cascade_count;
        
        // Check if we need to update the static cache
        bool light_changed = bud::math::length(view.light_dir - last_light_dir) > 0.001f;
        bool need_update = !cache_initialized || light_changed || !config.cache_shadows; 
        
        if (config.cache_shadows) {
            if (light_changed) last_light_dir = view.light_dir;

            // 1. Create Persistent Cache Texture if missing (or resize needed)
            if (stored_rhi && (!static_cache_texture || static_cache_texture->width != desc.width)) {
                static_cache_texture = stored_rhi->create_texture(desc, nullptr, 0);
                std::println("[CSMShadowPass] Created Static Shadow Cache ({}x{})", desc.width, desc.height);
                need_update = true;
            }
        }

        auto shadow_map_h = std::make_shared<RGHandle>(); // Active Map (Transient)
        
        // [CSM] 2. Static Cache Update Pass (Only if needed)
        RGHandle static_cache_h;
        bool valid_cache = config.cache_shadows && static_cache_texture;
        
        if (valid_cache) {
            // Import persistent texture into graph
            static_cache_h = rg.import_texture("StaticShadowCache", static_cache_texture, ResourceState::ShaderResource); 

            if (need_update) {
                rg.add_pass("CSM Static Update",
                    [&](RGBuilder& builder) {
                        builder.write(static_cache_h, ResourceState::DepthWrite);
                        return static_cache_h;
                    },
                    [=, &rg, &scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
                        if (!pipeline) return;
                        // Render Static Objects into Static Cache
                        for (uint32_t i = 0; i < config.cascade_count; ++i) {
                            auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
                            bud::math::Frustum cascade_view_frustum_dbg;
                            cascade_view_frustum_dbg.update(cascade_light_view_proj);

                            RenderPassBeginInfo info;
                            info.depth_attachment = rg.get_texture(static_cache_h);
                            info.clear_depth = true;
                            info.base_array_layer = i;
                            info.layer_count = 1;
                            
                            rhi->cmd_begin_render_pass(cmd, info);
                            rhi->cmd_bind_pipeline(cmd, pipeline);
                            rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);
                            rhi->cmd_set_viewport(cmd, (float)config.shadow_map_size, (float)config.shadow_map_size);
                            rhi->cmd_set_scissor(cmd, config.shadow_map_size, config.shadow_map_size);

                            // 想要世界偏移恒定，Bias 就要除以 级联缩放倍率。
                            // 因为 ShadowMap 坐标系被缩放了，Bias 需要更精细才能抵消放大的副作用。
                            auto cascade_scale = std::pow(3.0f, static_cast<float>(i));
                            auto inv_scale = 1.0f / cascade_scale;

                            auto base_bias = config.shadow_bias_constant * inv_scale;
                            auto slope_bias = config.shadow_bias_slope * inv_scale;
                            //auto cascade_bias = base_bias;
                            //auto current_slope_bias = slope_bias / std::pow(2.0f, static_cast<float>(i));
                            rhi->cmd_set_depth_bias(cmd, base_bias, 0.0f, slope_bias);

                            struct PushConsts {
                                bud::math::mat4 light_view_proj;
                                bud::math::mat4 model;
                                bud::math::vec4 light_dir;
                            } push_consts;
                            push_consts.light_view_proj = cascade_light_view_proj;
                            push_consts.light_dir = bud::math::vec4(bud::math::normalize(view.light_dir), 0.0f);

                            for (const auto& entity : scene.entities) {
                                if (!entity.is_static) continue; // ONLY STATIC
                                if (entity.mesh_index >= meshes.size()) continue;
                                const auto& mesh = meshes[entity.mesh_index];
                                if (!mesh.is_valid()) continue;

                                // Culling (Duplicate logic, could refactor)
                                bud::math::BoundingSphere world_sphere = mesh.sphere.transform(entity.transform);
                                if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;
                                bud::math::AABB world_aabb = mesh.aabb.transform(entity.transform);
                                if (!bud::math::intersect_aabb_frustum(world_aabb, cascade_view_frustum_dbg)) continue;
                                
                                push_consts.model = entity.transform;
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
                if (valid_cache) builder.read(static_cache_h); // Dep on cache
                return *shadow_map_h;
            },
            [=, &rg, &scene, &meshes, &view](RHI* rhi, CommandHandle cmd) {
                if (!pipeline) return;

                Texture* active_map = rg.get_texture(*shadow_map_h);
                bool did_copy = false;

                // Copy Cache if available
                if (valid_cache && cache_initialized) { 
                    // Whether we just updated it or it was already valid, we copy it to active map.
                    Texture* static_map = rg.get_texture(static_cache_h);
                    
                    // Barriers: static needs to be source, active needs to be dest
                    rhi->resource_barrier(cmd, static_map, ResourceState::ShaderResource, ResourceState::TransferSrc); 
                    rhi->resource_barrier(cmd, active_map, ResourceState::DepthWrite, ResourceState::TransferDst);
                    
                    rhi->cmd_copy_image(cmd, static_map, active_map);

                    // Transition back
                    rhi->resource_barrier(cmd, active_map, ResourceState::TransferDst, ResourceState::DepthWrite);
                    rhi->resource_barrier(cmd, static_map, ResourceState::TransferSrc, ResourceState::ShaderResource); 
                    
                    did_copy = true;
                }

                for (uint32_t i = 0; i < config.cascade_count; ++i) {
                    auto cascade_light_view_proj = view.cascade_view_proj_matrices[i];
                    bud::math::Frustum cascade_view_frustum_dbg;
                    cascade_view_frustum_dbg.update(cascade_light_view_proj);

                    RenderPassBeginInfo info;
                    info.depth_attachment = active_map;
                    info.clear_depth = !did_copy;
                    info.base_array_layer = i;
                    info.layer_count = 1;
                    
                    rhi->cmd_begin_render_pass(cmd, info);
                    rhi->cmd_bind_pipeline(cmd, pipeline);
                    rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);
                    rhi->cmd_set_viewport(cmd, (float)config.shadow_map_size, (float)config.shadow_map_size);
                    rhi->cmd_set_scissor(cmd, config.shadow_map_size, config.shadow_map_size);

                    auto cascade_scale = std::pow(3.0f, static_cast<float>(i));
                    auto inv_scale = 1.0f / cascade_scale;

                    auto base_bias = config.shadow_bias_constant * inv_scale;
                    auto slope_bias = config.shadow_bias_slope * inv_scale;
                    //auto cascade_bias = base_bias;
                    //auto current_slope_bias = slope_bias / std::pow(2.0f, static_cast<float>(i));
                    rhi->cmd_set_depth_bias(cmd, base_bias, 0.0f, slope_bias);

                    struct PushConsts {
                        bud::math::mat4 light_view_proj;
                        bud::math::mat4 model;
                        bud::math::vec4 light_dir;
                    } push_consts;
                    push_consts.light_view_proj = cascade_light_view_proj;
                    push_consts.light_dir = bud::math::vec4(bud::math::normalize(view.light_dir), 0.0f);

                    for (const auto& entity : scene.entities) {
                        if (did_copy && entity.is_static) continue; // Skip static ONLY if we successfully copied cache
                        if (entity.mesh_index >= meshes.size()) continue;

                        const auto& mesh = meshes[entity.mesh_index];
                        if (!mesh.is_valid()) continue;

                        // Culling
                        bud::math::BoundingSphere world_sphere = mesh.sphere.transform(entity.transform);
                        if (!bud::math::intersect_sphere_frustum(world_sphere, cascade_view_frustum_dbg)) continue;

                        bud::math::AABB world_aabb = mesh.aabb.transform(entity.transform);
                        if (!bud::math::intersect_aabb_frustum(world_aabb, cascade_view_frustum_dbg)) continue;
                        
                        push_consts.model = entity.transform;
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
        const bud::scene::Scene& scene, const SceneView& view,
        const std::vector<RenderMesh>& meshes)
    {
        TextureDesc depth_desc;
        depth_desc.width = (uint32_t)view.viewport_width;
        depth_desc.height = (uint32_t)view.viewport_height;
        depth_desc.format = TextureFormat::D32_FLOAT;

        // 使用 shared_ptr 来在两个 lambda 之间共享 Handle
        auto depth_h = std::make_shared<RGHandle>();

        rg.add_pass("Main Lighting Pass",
            [=](RGBuilder& builder) {
                builder.write(backbuffer, ResourceState::RenderTarget);
                builder.read(shadow_map, ResourceState::ShaderResource);
                
                *depth_h = builder.create("MainDepth", depth_desc);
                builder.write(*depth_h, ResourceState::DepthWrite);
                return *depth_h;
            },
            [=, &rg, &scene, &meshes, this](RHI* rhi, CommandHandle cmd) {
                // std::println("[MainPass] Execute started, entities={}", scene.entities.size());
                RenderPassBeginInfo info;
                info.color_attachments.push_back(rg.get_texture(backbuffer));
                info.depth_attachment = rg.get_texture(*depth_h);
                info.clear_color = true;
                info.clear_color_value = { 0.1f, 0.1f, 0.1f, 1.0f };
                info.clear_depth = true;

                // std::println("[MainPass] Beginning render pass...");
                rhi->cmd_begin_render_pass(cmd, info);

                if (pipeline) {
                    rhi->cmd_bind_pipeline(cmd, pipeline);
                }

                rhi->cmd_set_viewport(cmd, view.viewport_width, view.viewport_height);
                rhi->cmd_set_scissor(cmd, (uint32_t)view.viewport_width, (uint32_t)view.viewport_height);
                rhi->cmd_set_depth_bias(cmd, 0.0f, 0.0f, 0.0f);

                // [FIX] Bind Shadow Map to Global Descriptor Set
                rhi->update_global_shadow_map(rg.get_texture(shadow_map));

                rhi->update_global_uniforms(rhi->get_current_image_index(), view);
                if (pipeline) {
                    rhi->cmd_bind_descriptor_set(cmd, pipeline, 0);
                }

                for (const auto& entity : scene.entities) {
                    if (entity.mesh_index >= meshes.size()) continue;

                    const auto& mesh = meshes[entity.mesh_index];
                    if (!mesh.is_valid()) continue;

                    // [FIX] Update per-entity model matrix via push constant
                    struct PushVars {
                        bud::math::mat4 model;
                    } push_vars;
                    push_vars.model = entity.transform;
                    rhi->cmd_push_constants(cmd, pipeline, sizeof(PushVars), &push_vars);

                    rhi->cmd_bind_vertex_buffer(cmd, mesh.vertex_buffer.internal_handle);
                    rhi->cmd_bind_index_buffer(cmd, mesh.index_buffer.internal_handle);

                    rhi->cmd_draw_indexed(cmd, mesh.index_count, 1, 0, 0, 0);

                }

                rhi->cmd_end_render_pass(cmd);
            }
        );
    }
}
