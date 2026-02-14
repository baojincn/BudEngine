#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>
#include <print>
#include <cstring>

#include "src/graphics/bud.graphics.renderer.hpp"

#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/graphics/bud.graphics.graph.hpp"
#include "src/graphics/bud.graphics.passes.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/io/bud.io.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"

namespace bud::graphics {

	Renderer::Renderer(RHI* rhi, bud::io::AssetManager* asset_manager, bud::threading::TaskScheduler* task_scheduler)
		: rhi(rhi), render_graph(rhi), asset_manager(asset_manager), task_scheduler(task_scheduler) {
		upload_queue = std::make_shared<UploadQueue>();
		csm_pass = std::make_unique<CSMShadowPass>();
		main_pass = std::make_unique<MainPass>();
		csm_pass->init(rhi, render_config);
		main_pass->init(rhi, render_config);
	}

	Renderer::~Renderer() {
		flush_upload_queue();
		upload_queue.reset();

		if (csm_pass) {
			csm_pass->shutdown();
		}

		for (auto& mesh : meshes) {
			if (mesh.vertex_buffer.is_valid())
				rhi->destroy_buffer(mesh.vertex_buffer);

			if (mesh.index_buffer.is_valid())
				rhi->destroy_buffer(mesh.index_buffer);
		}
	}

	std::vector<bud::math::AABB> Renderer::get_mesh_bounds_snapshot() const {
		std::lock_guard lock(mesh_bounds_mutex);
		return mesh_bounds;
	}

	MeshAssetHandle Renderer::upload_mesh(const bud::io::MeshData& mesh_data) {
		if (mesh_data.vertices.empty())
			return MeshAssetHandle::invalid();

		std::println("\n[Renderer::upload_mesh] Processing mesh with {} subsets",
			mesh_data.subsets.size());

		std::vector<uint32_t> texture_slot_map;
		texture_slot_map.reserve(mesh_data.texture_paths.size());

		uint32_t base_material_id = 0;

		bud::math::AABB cpu_aabb;
		for (const auto& v : mesh_data.vertices) {
			cpu_aabb.merge(bud::math::vec3(v.pos[0], v.pos[1], v.pos[2]));
		}

		auto queue = upload_queue;
		auto queue_weak = std::weak_ptr<UploadQueue>(upload_queue);
		auto rhi_ptr = rhi;

		if (!mesh_data.texture_paths.empty()) {
			std::println("[upload_mesh] Allocating {} texture slots...",
				mesh_data.texture_paths.size());

			for (size_t i = 0; i < mesh_data.texture_paths.size(); ++i) {
				uint32_t current_slot = next_bindless_slot.fetch_add(1, std::memory_order_relaxed);
				texture_slot_map.push_back(current_slot);

				if (i == 0) {
					base_material_id = current_slot;
				}

				//std::println("  Texture[{}] '{}' -> Slot {}", i, mesh_data.texture_paths[i], current_slot);

				{
					std::lock_guard lock(queue->mutex);
					queue->commands.push_back([rhi_ptr, current_slot]() {
						rhi_ptr->update_bindless_texture(current_slot, rhi_ptr->get_fallback_texture());
					});
				}

				auto tex_path = mesh_data.texture_paths[i];

				// 发起异步加载
				asset_manager->load_image_async(tex_path,
					[queue_weak, rhi_ptr, current_slot, tex_path](bud::io::Image img) {
						auto img_ptr = std::make_shared<bud::io::Image>(std::move(img));

						auto queue_locked = queue_weak.lock();
						if (!queue_locked)
							return;

						std::lock_guard lock(queue_locked->mutex);
						queue_locked->commands.push_back([rhi_ptr, current_slot, tex_path, img_ptr]() {
							// --- Render Thread ---
							bud::graphics::TextureDesc desc{};
							desc.width = (uint32_t)img_ptr->width;
							desc.height = (uint32_t)img_ptr->height;
							desc.format = bud::graphics::TextureFormat::RGBA8_UNORM;
							desc.mips = static_cast<uint32_t>(std::floor(std::log2(std::max(desc.width, desc.height)))) + 1;

							auto tex = rhi_ptr->create_texture(desc, (const void*)img_ptr->pixels,
								(uint64_t)img_ptr->width * img_ptr->height * 4);
							rhi_ptr->set_debug_name(tex, ObjectType::Texture, tex_path);
							rhi_ptr->update_bindless_texture(current_slot, tex);

							//std::println("[Renderer] ✓ Texture BOUND: {} -> Slot {}", tex_path, current_slot);
						});
					}
				);
			}
		}

		auto mesh_data_copy = std::make_shared<bud::io::MeshData>(mesh_data);
		uint32_t assigned_mesh_id = 0;

		{
			std::lock_guard lock(queue->mutex);

			assigned_mesh_id = next_mesh_id.fetch_add(1, std::memory_order_relaxed);

			{
				std::lock_guard bounds_lock(mesh_bounds_mutex);
				if (mesh_bounds.size() <= assigned_mesh_id)
					mesh_bounds.resize(assigned_mesh_id + 1);

				mesh_bounds[assigned_mesh_id] = cpu_aabb;
			}

			queue->commands.push_back([this, mesh_data_copy, texture_slot_map, assigned_mesh_id, cpu_aabb]() {
				// --- Render Thread ---
				RenderMesh new_mesh;

				new_mesh.aabb = cpu_aabb;
				new_mesh.sphere.center = (cpu_aabb.min + cpu_aabb.max) * 0.5f;
				new_mesh.sphere.radius = bud::math::distance(cpu_aabb.max, new_mesh.sphere.center);
				new_mesh.index_count = (uint32_t)mesh_data_copy->indices.size();

				uint64_t v_size = mesh_data_copy->vertices.size() * sizeof(bud::io::MeshData::Vertex);
				uint64_t i_size = mesh_data_copy->indices.size() * sizeof(uint32_t);

				new_mesh.vertex_buffer = rhi->create_gpu_buffer(v_size, ResourceState::VertexBuffer);
				new_mesh.index_buffer = rhi->create_gpu_buffer(i_size, ResourceState::IndexBuffer);

				auto v_stage = rhi->create_upload_buffer(v_size);
				auto i_stage = rhi->create_upload_buffer(i_size);

				std::memcpy(v_stage.mapped_ptr, mesh_data_copy->vertices.data(), v_size);
				std::memcpy(i_stage.mapped_ptr, mesh_data_copy->indices.data(), i_size);

				rhi->copy_buffer_immediate(v_stage, new_mesh.vertex_buffer, v_size);
				rhi->copy_buffer_immediate(i_stage, new_mesh.index_buffer, i_size);

				rhi->destroy_buffer(v_stage);
				rhi->destroy_buffer(i_stage);

				if (!mesh_data_copy->subsets.empty()) {
					//std::println("[upload_mesh] Mesh[{}]: Processing {} subsets", assigned_mesh_id, mesh_data_copy->subsets.size());

					for (size_t i = 0; i < mesh_data_copy->subsets.size(); ++i) {
						const auto& subset = mesh_data_copy->subsets[i];
						SubMesh sub;
						sub.index_start = subset.index_start;
						sub.index_count = subset.index_count;

						if (subset.material_index < texture_slot_map.size()) {
							sub.material_id = texture_slot_map[subset.material_index];
							//std::println("  Subset[{}]: mat_idx={} -> GPU_slot={}", i, subset.material_index, sub.material_id);
						}
						else {
							std::println(stderr,
								"  Subset[{}]: INVALID mat_idx={} (max: {}) -> using fallback!",
								i, subset.material_index, texture_slot_map.size());
							sub.material_id = 0;
						}

						// --- Build per-submesh bounds ---
						bud::math::AABB sub_aabb;
						for (uint32_t k = 0; k < sub.index_count; ++k) {
							uint32_t vi = mesh_data_copy->indices[sub.index_start + k];
							const auto& v = mesh_data_copy->vertices[vi];
							sub_aabb.merge(bud::math::vec3(v.pos[0], v.pos[1], v.pos[2]));
						}

						sub.aabb = sub_aabb;
						sub.sphere.center = (sub_aabb.min + sub_aabb.max) * 0.5f;
						sub.sphere.radius = bud::math::distance(sub_aabb.max, sub.sphere.center);

						new_mesh.submeshes.push_back(sub);
					}
				}
				else {
					std::println(stderr, "[upload_mesh] Mesh[{}]: NO SUBSETS! Using fallback",
						assigned_mesh_id);
					SubMesh sub;
					sub.index_start = 0;
					sub.index_count = (uint32_t)mesh_data_copy->indices.size();
					sub.material_id = texture_slot_map.empty() ? 0 : texture_slot_map[0];
					new_mesh.submeshes.push_back(sub);
				}

				meshes.push_back(std::move(new_mesh));

				std::println("[Renderer] Mesh uploaded. Count: {}", meshes.size());
			});
		}

		return { assigned_mesh_id, base_material_id };
	}

	void Renderer::flush_upload_queue() {
		auto queue = upload_queue;
		if (!queue)
			return;

		std::vector<std::function<void()>> commands_to_run;
		{
			std::lock_guard lock(queue->mutex);
			if (queue->commands.empty())
				return;

			commands_to_run.swap(queue->commands);
		}

		for (const auto& rhi_cmd : commands_to_run) {
			rhi_cmd();
		}
	}



	void Renderer::render(const bud::graphics::RenderScene& scene, SceneView& scene_view) {
		// 先处理所有挂起的上传任务
		flush_upload_queue();

		size_t instance_count = scene.instance_count.load(std::memory_order_relaxed);
		if (instance_count == 0) return;

		bud::math::AABB scene_aabb;

		for (size_t i = 0; i < instance_count; ++i) {
			scene_aabb.merge(scene.world_aabbs[i]);
		}

		// 更新阴影级联
		update_cascades(scene_view, render_config, scene_aabb);

		bud::math::Frustum camera_frustum;
		camera_frustum.update(scene_view.view_proj_matrix);

		// Prepass: compute draw offsets per instance
		std::vector<uint32_t> draw_offsets(instance_count + 1);
		size_t total_draw_count = 0;

		for (size_t i = 0; i < instance_count; ++i) {
			draw_offsets[i] = (uint32_t)total_draw_count;

			uint32_t mesh_id = scene.mesh_indices[i];
			if (mesh_id >= meshes.size() || !meshes[mesh_id].is_valid()) {
				continue;
			}

			const auto& mesh = meshes[mesh_id];
			uint32_t submesh_count = mesh.submeshes.empty() ? 1u : (uint32_t)mesh.submeshes.size();
			total_draw_count += submesh_count;
		}

		draw_offsets[instance_count] = (uint32_t)total_draw_count;

		if (sort_list.size() < total_draw_count)
			sort_list.resize(total_draw_count);

		bud::threading::Counter key_gen_signal;
		constexpr size_t CHUNK_SIZE = 64;

		task_scheduler->ParallelFor(instance_count, CHUNK_SIZE,
			[&](size_t start_exclusive, size_t end_exclusive) {
				for (size_t i = start_exclusive; i < end_exclusive; ++i) {
					uint32_t draw_start = draw_offsets[i];
					uint32_t draw_end = draw_offsets[i + 1];
					uint32_t draw_count = draw_end - draw_start;

					if (draw_count == 0)
						continue;

					const auto& world_matrix = scene.world_matrices[i];
					const auto& aabb = scene.world_aabbs[i];

					// coarse cull at instance level
					if (!bud::math::intersect_aabb_frustum(aabb, camera_frustum)) {
						for (uint32_t j = 0; j < draw_count; ++j) {
							auto& item = sort_list[draw_start + j];
							item.key = UINT64_MAX;
							item.entity_index = (uint32_t)i;
							item.submesh_index = j;
						}
						continue;
					}

					uint32_t mesh_id = scene.mesh_indices[i];
					if (mesh_id >= meshes.size()) {
						for (uint32_t j = 0; j < draw_count; ++j) {
							auto& item = sort_list[draw_start + j];
							item.key = UINT64_MAX;
							item.entity_index = (uint32_t)i;
							item.submesh_index = j;
						}
						continue;
					}

					const auto& mesh = meshes[mesh_id];

					auto mesh_pos = bud::math::vec3(world_matrix[3]); // 提取平移部分作为实体位置
					auto distance = bud::math::distance2(mesh_pos, scene_view.camera_position);

					uint32_t depth_key = 0;
					auto depth_normalized = std::clamp(distance / (scene_view.far_plane * scene_view.far_plane), 0.0f, 1.0f);
					depth_key = static_cast<uint32_t>(depth_normalized * 0x3FFFF); // 18 bits for depth

					if (!mesh.submeshes.empty()) {
						for (uint32_t j = 0; j < draw_count; ++j) {
							const auto& sub = mesh.submeshes[j];
							auto world_sub_aabb = sub.aabb.transform(world_matrix);

							auto& item = sort_list[draw_start + j];
							item.entity_index = (uint32_t)i;
							item.submesh_index = j;

							if (!bud::math::intersect_aabb_frustum(world_sub_aabb, camera_frustum)) {
								item.key = UINT64_MAX;
								continue;
							}

							item.key = DrawKey::generate_opaque(0, 0, sub.material_id, mesh_id, depth_key);
						}
					}
					else {
						auto& item = sort_list[draw_start];
						item.entity_index = (uint32_t)i;
						item.submesh_index = UINT32_MAX;

						uint32_t material_id = scene.material_indices[i];
						item.key = DrawKey::generate_opaque(0, 0, material_id, mesh_id, depth_key);
					}
				}
			},
			&key_gen_signal
		);

		task_scheduler->wait_for_counter(key_gen_signal);

		std::sort(sort_list.begin(), sort_list.begin() + total_draw_count,
			[](const SortItem& a, const SortItem& b) { return a.key < b.key; }
		);

		// 使用二分查找找到第一个 UINT64_MAX 的位置, 计算可见物体数量 (剔除 Key == UINT64_MAX 的物体)
		auto it = std::lower_bound(sort_list.begin(), sort_list.begin() + total_draw_count, UINT64_MAX,
			[](const SortItem& item, uint64_t val) { return item.key < val; }
		);

		size_t visible_count = std::distance(sort_list.begin(), it);

		// 如果没有东西可见，直接返回，不录制 CommandBuffer
		if (visible_count == 0)
			return;

		//std::println("[Culling] total_draw_count={} visible_count={}", total_draw_count, visible_count);

		auto cmd = rhi->begin_frame();
		if (!cmd)
			return;

		rhi->set_render_config(render_config);

		auto swapchain_tex = rhi->get_current_swapchain_texture();
		auto back_buffer = render_graph.import_texture("Backbuffer", swapchain_tex, ResourceState::RenderTarget);

		auto shadow_map = csm_pass->add_to_graph(render_graph, scene_view, render_config, scene, meshes);

		// sort_list 传进去，按照这个顺序画
		main_pass->add_to_graph(render_graph, shadow_map, back_buffer, scene, scene_view, render_config, meshes, sort_list, visible_count);

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
		auto cam_near = view.near_plane;
		auto cam_far = view.far_plane;
		auto shadow_far = config.shadow_far_plane;
		if (shadow_far > cam_far) {
			shadow_far = cam_far;
		}

		const uint32_t cascade_count = std::min(config.cascade_count, MAX_CASCADES);

		auto lambda = config.cascade_split_lambda;

		// 1. Calculate Split Depths (Log-Linear)
		float cascade_splits[MAX_CASCADES];
		for (uint32_t i = 0; i < cascade_count; ++i) {
			auto p = (float)(i + 1) / (float)cascade_count;
			auto log = cam_near * std::pow(shadow_far / cam_near, p);
			auto uniform = cam_near + (shadow_far - cam_near) * p;
			auto d = lambda * log + (1.0f - lambda) * uniform;
			cascade_splits[i] = (d - cam_near) / (cam_far - cam_near);
			view.cascade_split_depths[i] = d;
		}

		for (uint32_t i = cascade_count; i < MAX_CASCADES; ++i) {
			view.cascade_split_depths[i] = view.far_plane;
			view.cascade_view_proj_matrices[i] = bud::math::mat4(1.0f);
		}

		// 2. Calculate Matrices
		auto inv_cam_matrix = bud::math::inverse(view.proj_matrix * view.view_matrix);
		auto L = bud::math::normalize(view.light_dir);
		auto light_view_matrix = bud::math::lookAt(L * 100.0f, bud::math::vec3(0.0f), bud::math::vec3(0.0f, 1.0f, 0.0f));

		auto last_split = 0.0f;
		for (uint32_t i = 0; i < cascade_count; ++i) {
			auto split = cascade_splits[i];

			const float ndc_near = config.reversed_z ? 1.0f : 0.0f;
			const float ndc_far = config.reversed_z ? 0.0f : 1.0f;
			bud::math::vec3 frustum_corners[8] = {
				{-1.0f,  1.0f, ndc_near}, { 1.0f,  1.0f, ndc_near}, { 1.0f, -1.0f, ndc_near}, {-1.0f, -1.0f, ndc_near},
				{-1.0f,  1.0f, ndc_far}, { 1.0f,  1.0f, ndc_far}, { 1.0f, -1.0f, ndc_far}, {-1.0f, -1.0f, ndc_far},
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
			auto up = (std::abs(L.y) > 0.99f) ? bud::math::vec3(0.0f, 0.0f, 1.0f) : bud::math::vec3(0.0f, 1.0f, 0.0f);
			auto light_rot_matrix = bud::math::lookAt(bud::math::vec3(0.0f), -L, up);

			// 将中心点转到光照空间
			auto center_of_light_space = light_rot_matrix * bud::math::vec4(frustum_center, 1.0f);

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
			auto light_scene_aabb = scene_aabb.transform(light_rot_matrix);

			// Z 轴方向：在 View Space 中，相机看 -Z。
			// 物体越远 Z 越负。light_scene_aabb.min.z 是最远的，max.z 是最近的。
			float near_z = -light_scene_aabb.max.z - 100.0f; // 场景最近端 (加缓冲)
			float far_z = -light_scene_aabb.min.z + 100.0f; // 场景最远端 (加缓冲)

			// 构建最终矩阵
			auto light_proj_matrix = config.reversed_z
				? bud::math::ortho_vk_reversed(min_x, max_x, min_y, max_y, near_z, far_z)
				: bud::math::ortho_vk(min_x, max_x, min_y, max_y, near_z, far_z);


			view.cascade_view_proj_matrices[i] = light_proj_matrix * light_rot_matrix;

			last_split = split;
		}
	}
}
