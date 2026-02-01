#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>
#include <print>
#include <cstring> // for std::memcpy

#include "src/graphics/bud.graphics.renderer.hpp"

#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/core/bud.math.hpp"



namespace bud::graphics {

    Renderer::Renderer(RHI* rhi, bud::io::AssetManager* asset_manager)
		: rhi(rhi), render_graph(rhi), asset_manager(asset_manager) {
        csm_pass = std::make_unique<CSMShadowPass>();
        main_pass = std::make_unique<MainPass>();
        csm_pass->init(rhi);
        main_pass->init(rhi);
    }

    Renderer::~Renderer() {
        for (auto& mesh : meshes) {
            if (mesh.vertex_buffer.is_valid())
                rhi->destroy_buffer(mesh.vertex_buffer);

            if (mesh.index_buffer.is_valid())
                rhi->destroy_buffer(mesh.index_buffer);
        }
    }

    uint32_t Renderer::upload_mesh(const bud::io::MeshData& mesh_data) {
        if (mesh_data.vertices.empty()) return 0;

        // 1. 创建 Staging Buffers (CPU 可写)
        uint64_t v_size = mesh_data.vertices.size() * sizeof(bud::io::MeshData::Vertex);
        uint64_t i_size = mesh_data.indices.size() * sizeof(uint32_t);

        MemoryBlock v_stage = rhi->create_upload_buffer(v_size);
        MemoryBlock i_stage = rhi->create_upload_buffer(i_size);
        
        // 2. 写入数据 & 计算 AABB
        std::memcpy(v_stage.mapped_ptr, mesh_data.vertices.data(), v_size);
        std::memcpy(i_stage.mapped_ptr, mesh_data.indices.data(), i_size);

        bud::math::AABB aabb;
        for (const auto& v : mesh_data.vertices) {
            aabb.merge(bud::math::vec3(v.pos[0], v.pos[1], v.pos[2]));
        }


        // 3. 创建 GPU Buffers (Device Local)
        RenderMesh render_mesh;
        render_mesh.vertex_buffer = rhi->create_gpu_buffer(v_size, ResourceState::VertexBuffer);
        render_mesh.index_buffer = rhi->create_gpu_buffer(i_size, ResourceState::IndexBuffer);
        render_mesh.index_count = (uint32_t)mesh_data.indices.size();
        render_mesh.aabb = aabb;

        // [CSM] Calculate Bounding Sphere
        render_mesh.sphere.center = (aabb.min + aabb.max) * 0.5f;
        render_mesh.sphere.radius = bud::math::distance(aabb.max, render_mesh.sphere.center);


        // 4. 执行 GPU 拷贝
        rhi->copy_buffer_immediate(v_stage, render_mesh.vertex_buffer, v_size);
        rhi->copy_buffer_immediate(i_stage, render_mesh.index_buffer, i_size);

        std::println("[Renderer] Uploaded mesh: {} vertices, {} indices", mesh_data.vertices.size(), mesh_data.indices.size());
        
        for (size_t i = 0; i < mesh_data.texture_paths.size(); ++i) {
            auto bindless_index = static_cast<uint32_t>(i + 1);
            rhi->update_bindless_texture(bindless_index, rhi->get_fallback_texture());

            auto tex_path = mesh_data.texture_paths[i];
            asset_manager->load_image_async(tex_path,
                [this, bindless_index, tex_path](bud::io::Image img) {
                    bud::graphics::TextureDesc desc{};
                    desc.width = (uint32_t)img.width;
                    desc.height = (uint32_t)img.height;
                    desc.format = bud::graphics::TextureFormat::RGBA8_UNORM;
                    desc.mips = static_cast<uint32_t>(std::floor(std::log2(std::max(desc.width, desc.height)))) + 1;

                    auto tex = rhi->create_texture(desc, (const void*)img.pixels, (uint64_t)img.width * img.height * 4);

                    rhi->set_debug_name(tex, ObjectType::Texture, tex_path);

                    rhi->update_bindless_texture(bindless_index, tex);

                    std::println("[Renderer] Texture streamed in: {} -> Slot {}", tex_path, bindless_index);
                }
            );
        }

        rhi->destroy_buffer(v_stage);
        rhi->destroy_buffer(i_stage);

        meshes.push_back(std::move(render_mesh));
        return static_cast<uint32_t>(meshes.size() - 1);
    }

    void Renderer::render(const bud::scene::Scene& scene, SceneView& scene_view) {
        bud::math::AABB scene_aabb;
        for (const auto& entity : scene.entities) {
            if (entity.mesh_index < meshes.size() && meshes[entity.mesh_index].is_valid()) {
                scene_aabb.merge(meshes[entity.mesh_index].aabb.transform(entity.transform));
            }
        }

        update_cascades(scene_view, render_config, scene_aabb);

        auto cmd = rhi->begin_frame();

        if (!cmd)
            return;

        auto swapchain_tex = rhi->get_current_swapchain_texture();
        auto back_buffer = render_graph.import_texture("Backbuffer", swapchain_tex, ResourceState::RenderTarget);

        auto shadow_map = csm_pass->add_to_graph(render_graph, scene_view, render_config, scene, meshes);

        // Main Pass
        main_pass->add_to_graph(render_graph, shadow_map, back_buffer, scene, scene_view, meshes);

        render_graph.compile();

        rhi->resource_barrier(cmd, swapchain_tex, ResourceState::Undefined, ResourceState::RenderTarget);

        render_graph.execute(cmd);

        rhi->resource_barrier(cmd, swapchain_tex, ResourceState::RenderTarget, ResourceState::Present);
        rhi->end_frame(cmd);
    }

    void Renderer::set_config(const RenderConfig& config) {
        render_config = config;
    }

    const RenderConfig& Renderer::get_config() const {
        return render_config;
    }

    void Renderer::update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb) {
        auto near = view.near_plane;
        auto far = view.far_plane;
        auto lambda = config.cascade_split_lambda;

        // 1. Calculate Split Depths (Log-Linear)
        float cascade_splits[MAX_CASCADES];
        for (uint32_t i = 0; i < config.cascade_count; ++i) {
            auto p = (float)(i + 1) / (float)config.cascade_count;
            auto log = near * std::pow(far / near, p);
            auto uniform = near + (far - near) * p;
            auto d = lambda * log + (1.0f - lambda) * uniform;
            cascade_splits[i] = (d - near) / (far - near);
            view.cascade_split_depths[i] = d;
        }

        // 2. Calculate Matrices
        bud::math::mat4 inv_cam_matrix = bud::math::inverse(view.proj_matrix * view.view_matrix);
        bud::math::vec3 L = bud::math::normalize(view.light_dir); 
        bud::math::mat4 light_view_matrix = bud::math::lookAt(L * 100.0f, bud::math::vec3(0.0f), bud::math::vec3(0.0f, 1.0f, 0.0f));

        auto last_split = 0.0f;
        for (uint32_t i = 0; i < config.cascade_count; ++i) {
            auto split = cascade_splits[i];

            bud::math::vec3 frustum_corners[8] = {
                {-1.0f,  1.0f, 0.0f}, { 1.0f,  1.0f, 0.0f}, { 1.0f, -1.0f, 0.0f}, {-1.0f, -1.0f, 0.0f},
                {-1.0f,  1.0f, 1.0f}, { 1.0f,  1.0f, 1.0f}, { 1.0f, -1.0f, 1.0f}, {-1.0f, -1.0f, 1.0f},
            };
            for (uint32_t j = 0; j < 4; ++j) {
                auto vec_near = inv_cam_matrix * bud::math::vec4(frustum_corners[j], 1.0f);
                vec_near /= vec_near.w;
                auto vec_far = inv_cam_matrix * bud::math::vec4(frustum_corners[j + 4], 1.0f);
                vec_far /= vec_far.w;

                frustum_corners[j] = bud::math::vec3(vec_near + (vec_far - vec_near) * last_split);
                frustum_corners[j + 4] = bud::math::vec3(vec_near + (vec_far - vec_near) * split);
            }

            // 1. 计算视锥体切片的中心 (用于定位)
            bud::math::vec3 frustum_center(0.0f);
            for (const auto& v : frustum_corners)
                frustum_center += v;

            frustum_center /= 8.0f;

            // 2. 计算包围球半径 (用于固定投影大小)
            auto radius = 0.0f;
            for (const auto& v : frustum_corners)
                radius = std::max(radius, bud::math::length(v - frustum_center));

            radius = std::max(radius, 50.0f);
            radius *= 2;

            // 向上取整半径，消除浮点抖动，保证 absolute stability
            radius = std::ceil(radius * 16.0f) / 16.0f;

            // 3. 构建仅包含旋转的光照 View 矩阵 (消除位置抖动)
            bud::math::vec3 up = (std::abs(L.y) > 0.99f) ? bud::math::vec3(0.0f, 0.0f, 1.0f) : bud::math::vec3(0.0f, 1.0f, 0.0f);
            bud::math::mat4 light_rot_matrix = bud::math::lookAt(bud::math::vec3(0.0f), -L, up);

            // 将中心点转到光照空间
            bud::math::vec4 center_of_light_space = light_rot_matrix * bud::math::vec4(frustum_center, 1.0f);

            // 4. 计算固定大小的纹素尺寸
            float diameter = radius * 2.0f;
            float shadow_map_size = (float)config.shadow_map_size;
            float world_units_per_texel = diameter / shadow_map_size;

            // 5. Texel Snapping (对齐中心点)
            float snapped_x = std::floor(center_of_light_space.x / world_units_per_texel) * world_units_per_texel;
            float snapped_y = std::floor(center_of_light_space.y / world_units_per_texel) * world_units_per_texel;

            // 6. 构建正交投影 (基于对齐后的中心)
            float min_x = snapped_x - radius;
            float max_x = snapped_x + radius;
            float min_y = snapped_y - radius;
            float max_y = snapped_y + radius;

            // 7. Z 轴裁剪 (Scene Fitting)
            // 将场景 AABB 转到光照空间，用于确定准确的 Near/Far
            bud::math::AABB light_scene_aabb = scene_aabb.transform(light_rot_matrix);

            // Z 轴方向：在 View Space 中，相机看 -Z。
            // 物体越远 Z 越负。light_scene_aabb.min.z 是最远的，max.z 是最近的。
            // 我们需要包含整个场景：
            float near_z = -light_scene_aabb.max.z - 100.0f; // 场景最近端 (加缓冲)
            float far_z = -light_scene_aabb.min.z + 100.0f; // 场景最远端 (加缓冲)

            // 构建最终矩阵
            bud::math::mat4 light_proj_matrix = bud::math::ortho_vk(min_x, max_x, min_y, max_y, near_z, far_z);

            
            view.cascade_view_proj_matrices[i] = light_proj_matrix * light_rot_matrix;

            last_split = split;
        }
    }
}
