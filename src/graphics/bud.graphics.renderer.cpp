#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>
#include <print>
#include <cstring>

#include "src/graphics/bud.graphics.renderer.hpp"

#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/graph/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"



namespace bud::graphics {

	Renderer::Renderer(RHI* rhi, bud::io::AssetManager* asset_manager, bud::threading::TaskScheduler* task_scheduler)
		: rhi(rhi), render_graph(rhi), asset_manager(asset_manager), task_scheduler(task_scheduler) {
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

	MeshAssetHandle Renderer::upload_mesh(const bud::io::MeshData& mesh_data) {
		if (mesh_data.vertices.empty())
			return { 0, 0 };

		// --------------------------------------------------------
		// 1. 纹理处理 (异步 + 延迟)
		// --------------------------------------------------------
		uint32_t base_material_id = 0;

		if (!mesh_data.texture_paths.empty()) {
			for (size_t i = 0; i < mesh_data.texture_paths.size(); ++i) {
				// 立即分配 Slot (原子操作，线程安全)
				uint32_t current_slot = next_bindless_slot.fetch_add(1, std::memory_order_relaxed);

				if (i == 0) {
					base_material_id = current_slot;
				}

				// [关键修复] 不要立即调 RHI！把“设置占位图”也加入队列
				{
					std::lock_guard lock(upload_mutex);
					pending_rhi_commands.push_back([this, current_slot]() {
						rhi->update_bindless_texture(current_slot, rhi->get_fallback_texture());
						});
				}

				auto tex_path = mesh_data.texture_paths[i];

				// 发起异步加载
				asset_manager->load_image_async(tex_path,
					[this, current_slot, tex_path](bud::io::Image img) {
						// 将 Image 转移到 shared_ptr 以便 Lambda 捕获 (解决 C2338)
						auto img_ptr = std::make_shared<bud::io::Image>(std::move(img));

						std::lock_guard lock(upload_mutex);
						pending_rhi_commands.push_back([this, current_slot, tex_path, img_ptr]() {
							// --- Render Thread ---
							bud::graphics::TextureDesc desc{};
							desc.width = (uint32_t)img_ptr->width;
							desc.height = (uint32_t)img_ptr->height;
							desc.format = bud::graphics::TextureFormat::RGBA8_UNORM;
							desc.mips = static_cast<uint32_t>(std::floor(std::log2(std::max(desc.width, desc.height)))) + 1;

							auto tex = rhi->create_texture(desc, (const void*)img_ptr->pixels, (uint64_t)img_ptr->width * img_ptr->height * 4);
							rhi->set_debug_name(tex, ObjectType::Texture, tex_path);
							rhi->update_bindless_texture(current_slot, tex);

							std::println("[Renderer] Texture streamed in: {} -> Slot {}", tex_path, current_slot);
							});
					}
				);
			}
		}

		// --------------------------------------------------------
		// 2. 网格处理 (延迟提交)
		// --------------------------------------------------------

		// 深拷贝 MeshData (因为原始数据可能在栈上，或者在 Lambda 执行前被释放)
		auto mesh_data_copy = std::make_shared<bud::io::MeshData>(mesh_data);

		uint32_t assigned_mesh_id = 0;

		{
			std::lock_guard lock(upload_mutex);

			// [核心逻辑] 在锁内分配 ID，确保这个 ID 对应着队列里的这次 push_back
			assigned_mesh_id = next_mesh_id++;

			pending_rhi_commands.push_back([this, mesh_data_copy]() {
				// --- Render Thread ---
				RenderMesh new_mesh;

				// 1. 计算 AABB
				bud::math::AABB aabb;
				for (const auto& v : mesh_data_copy->vertices) {
					aabb.merge(bud::math::vec3(v.pos[0], v.pos[1], v.pos[2]));
				}
				new_mesh.aabb = aabb;
				new_mesh.sphere.center = (aabb.min + aabb.max) * 0.5f;
				new_mesh.sphere.radius = bud::math::distance(aabb.max, new_mesh.sphere.center);
				new_mesh.index_count = (uint32_t)mesh_data_copy->indices.size();

				// 2. 创建 GPU Buffer
				uint64_t v_size = mesh_data_copy->vertices.size() * sizeof(bud::io::MeshData::Vertex);
				uint64_t i_size = mesh_data_copy->indices.size() * sizeof(uint32_t);

				new_mesh.vertex_buffer = rhi->create_gpu_buffer(v_size, ResourceState::VertexBuffer);
				new_mesh.index_buffer = rhi->create_gpu_buffer(i_size, ResourceState::IndexBuffer);

				// 3. Staging Copy
				auto v_stage = rhi->create_upload_buffer(v_size);
				auto i_stage = rhi->create_upload_buffer(i_size);

				std::memcpy(v_stage.mapped_ptr, mesh_data_copy->vertices.data(), v_size);
				std::memcpy(i_stage.mapped_ptr, mesh_data_copy->indices.data(), i_size);

				rhi->copy_buffer_immediate(v_stage, new_mesh.vertex_buffer, v_size);
				rhi->copy_buffer_immediate(i_stage, new_mesh.index_buffer, i_size);

				rhi->destroy_buffer(v_stage);
				rhi->destroy_buffer(i_stage);

				// 4. 提交到资源池
				meshes.push_back(std::move(new_mesh));

				std::println("[Renderer] Mesh uploaded. Count: {}", meshes.size());
				});
		}

		// 返回这个被“预定”的 ID，它是绝对安全的
		return { assigned_mesh_id, base_material_id };
	}


	void Renderer::flush_upload_queue() {
		std::vector<std::function<void()>> commands_to_run;
		{
			std::lock_guard lock(upload_mutex);
			if (pending_rhi_commands.empty())
				return;

			commands_to_run.swap(this->pending_rhi_commands);
		}

		// 在渲染线程执行累积的 RHI 操作
		for (const auto& rhi_cmd : commands_to_run) {
			rhi_cmd();
		}
	}



	void Renderer::render(const bud::graphics::RenderScene& scene, SceneView& scene_view) {
		// 先处理所有挂起的上传任务
		flush_upload_queue();

		// --------------------------------------------------------
		// 1. 准备阶段 (计算 Scene AABB)
		// --------------------------------------------------------
		size_t instance_count = scene.instance_count.load(std::memory_order_relaxed);
		if (instance_count == 0) return;

		bud::math::AABB scene_aabb;
		// 简单粗暴遍历 RenderScene 计算 AABB (用于 CSM 阴影范围)
		// 由于数据是连续内存，这一步其实很快
		for (size_t i = 0; i < instance_count; ++i) {
			scene_aabb.merge(scene.world_aabbs[i]);
		}

		// 更新阴影级联 (和原来保持一致)
		update_cascades(scene_view, render_config, scene_aabb);


		// --------------------------------------------------------
		// 2. 核心优化：并行生成排序键 (Key Gen & Sort)
		// --------------------------------------------------------
		static std::vector<SortItem> sort_list; // static 复用内存
		if (sort_list.size() < instance_count) sort_list.resize(instance_count);

		bud::threading::Counter key_gen_signal;
		constexpr size_t CHUNK_SIZE = 256;

		// [Task] 并行生成 Key
		task_scheduler->ParallelFor(instance_count, CHUNK_SIZE,
			[&](size_t start, size_t end) {
				for (size_t i = start; i < end; ++i) {
					uint32_t mat_id = scene.material_indices[i];
					uint32_t mesh_id = scene.mesh_indices[i];
					// 假设 pipeline_id 就在材质 ID 的高位，或者你可以查询
					// 这里为了演示简单，直接用 mat_id 生成 Key
					// 实际项目中：uint16_t pipe_id = get_pipeline(mat_id);
					sort_list[i].key = DrawKey::generate_opaque(0, 0, mat_id, (uint16_t)mesh_id);
					sort_list[i].entity_index = (uint32_t)i;
				}
			},
			&key_gen_signal
		);

		// [Main Thread] 帮忙一起算，并等待完成
		task_scheduler->wait_for_counter(key_gen_signal);

		// 注意：只排序前 instance_count 个有效元素
		std::sort(sort_list.begin(), sort_list.begin() + instance_count,
			[](const SortItem& a, const SortItem& b) { return a.key < b.key; }
		);


		// --------------------------------------------------------
		// 3. 构建 RenderGraph (和原来保持一致)
		// --------------------------------------------------------
		auto cmd = rhi->begin_frame();
		if (!cmd) return;

		rhi->set_render_config(render_config);

		auto swapchain_tex = rhi->get_current_swapchain_texture();
		auto back_buffer = render_graph.import_texture("Backbuffer", swapchain_tex, ResourceState::RenderTarget);

		// [Pass 1] CSM Shadow Pass
		// 阴影通常不需要材质排序（或者需要不同的排序逻辑），可以直接传 scene
		auto shadow_map = csm_pass->add_to_graph(render_graph, scene_view, render_config, scene, meshes);

		// [Pass 2] Main Pass (核心修改点！)
		// 我们把 排好序的 sort_list 传进去，让 MainPass 按照这个顺序画！
		main_pass->add_to_graph(render_graph, shadow_map, back_buffer, scene, scene_view, meshes, sort_list, instance_count);

		// --------------------------------------------------------
		// 4. 执行 Graph
		// --------------------------------------------------------
		render_graph.compile();

		rhi->resource_barrier(cmd, swapchain_tex, ResourceState::Undefined, ResourceState::RenderTarget);

		render_graph.execute(cmd); // 这里面的 MainPass 会读取 sort_list

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
		auto cam_near = view.near_plane;
		auto cam_far = view.far_plane;
		auto shadow_far = config.shadow_far_plane;
		if (shadow_far > cam_far) {
			shadow_far = cam_far;
		}

		auto lambda = config.cascade_split_lambda;

		// 1. Calculate Split Depths (Log-Linear)
		float cascade_splits[MAX_CASCADES];
		for (uint32_t i = 0; i < config.cascade_count; ++i) {
			auto p = (float)(i + 1) / (float)config.cascade_count;
			auto log = cam_near * std::pow(shadow_far / cam_near, p);
			auto uniform = cam_near + (shadow_far - cam_near) * p;
			auto d = lambda * log + (1.0f - lambda) * uniform;
			cascade_splits[i] = (d - cam_near) / (cam_far - cam_near);
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
