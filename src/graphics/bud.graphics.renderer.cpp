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
#include "src/core/bud.asset.types.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.sortkey.hpp"
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"

namespace bud::graphics {

	Renderer::Renderer(RHI* rhi, bud::io::AssetManager* asset_manager, bud::threading::TaskScheduler* task_scheduler)
		: rhi(rhi), render_graph(rhi), asset_manager(asset_manager), task_scheduler(task_scheduler) {
		upload_queue = std::make_shared<UploadQueue>();
		csm_pass = std::make_unique<CSMShadowPass>();
		z_prepass = std::make_unique<ZPrepass>();
		hiz_mip_pass = std::make_unique<HiZMipPass>();
		hiz_pass = std::make_unique<HiZCullingPass>();
		hiz_debug_pass = std::make_unique<HiZDebugPass>();
		main_pass = std::make_unique<MainPass>();
		ui_pass = std::make_unique<UIPass>();

		csm_pass->init(rhi, render_config, asset_manager);
		z_prepass->init(rhi, render_config, asset_manager);
		hiz_mip_pass->init(rhi, render_config, asset_manager);
		hiz_pass->init(rhi, render_config, asset_manager);
		hiz_debug_pass->init(rhi, render_config, asset_manager);
		main_pass->init(rhi, render_config, asset_manager);
		ui_pass->init(rhi, render_config, asset_manager);
	}

	Renderer::~Renderer() {
		flush_upload_queue();
		upload_queue.reset();

		if (csm_pass) {
			csm_pass->shutdown(rhi);
		}

		if (ui_pass) {
			ui_pass->shutdown(rhi);
		}

		if (hiz_pass) {
			hiz_pass->shutdown(rhi);
		}

		for (auto& mesh : meshes) {
			if (mesh.vertex_buffer.is_valid()) rhi->destroy_buffer(mesh.vertex_buffer);
			if (mesh.index_buffer.is_valid()) rhi->destroy_buffer(mesh.index_buffer);
			if (mesh.meshlet_buffer.is_valid()) rhi->destroy_buffer(mesh.meshlet_buffer);
			if (mesh.vertex_index_buffer.is_valid()) rhi->destroy_buffer(mesh.vertex_index_buffer);
			if (mesh.meshlet_index_buffer.is_valid()) rhi->destroy_buffer(mesh.meshlet_index_buffer);
			if (mesh.cull_data_buffer.is_valid()) rhi->destroy_buffer(mesh.cull_data_buffer);
		}
		
		for (auto& buf : indirect_instance_buffers) {
			if (buf.is_valid()) rhi->destroy_buffer(buf);
		}
		indirect_instance_buffers.clear();

		for (auto& buf : indirect_draw_buffers) {
			if (buf.is_valid()) rhi->destroy_buffer(buf);
		}

		indirect_draw_buffers.clear();

		for (auto& buf : stats_readback_buffers) {
			if (buf.is_valid()) rhi->destroy_buffer(buf);
		}

		stats_readback_buffers.clear();
	}

	std::vector<bud::math::AABB> Renderer::get_mesh_bounds_snapshot() const {
		std::lock_guard lock(mesh_bounds_mutex);
		return mesh_bounds;
	}

	std::vector<std::vector<bud::math::AABB>> Renderer::get_submesh_bounds_snapshot() const {
		std::lock_guard lock(mesh_bounds_mutex);
		std::vector<std::vector<bud::math::AABB>> result;
		result.reserve(meshes.size());
		for (const auto& mesh : meshes) {
			std::vector<bud::math::AABB> sub_aabbs;
			sub_aabbs.reserve(mesh.submeshes.size());
			for (const auto& sub : mesh.submeshes) {
				sub_aabbs.push_back(sub.aabb);
			}
			result.push_back(std::move(sub_aabbs));
		}
		return result;
	}

	MeshAssetHandle Renderer::upload_mesh(const bud::io::MeshData& mesh_data) {
		if (mesh_data.vertices.empty())
			return MeshAssetHandle::invalid();

		bud::print("\n[Renderer::upload_mesh] Processing mesh with {} subsets",
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
			bud::print("[upload_mesh] Allocating {} texture slots...",
				mesh_data.texture_paths.size());

			for (size_t i = 0; i < mesh_data.texture_paths.size(); ++i) {
				uint32_t current_slot = next_bindless_slot.fetch_add(1, std::memory_order_relaxed);
				texture_slot_map.push_back(current_slot);

				if (i == 0) {
					base_material_id = current_slot;
				}

				//bud::print("  Texture[{}] '{}' -> Slot {}", i, mesh_data.texture_paths[i], current_slot);

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
							bud::graphics::TextureDesc desc{};
							desc.width = (uint32_t)img_ptr->width;
							desc.height = (uint32_t)img_ptr->height;
							desc.format = bud::graphics::TextureFormat::RGBA8_UNORM;
							desc.mips = static_cast<uint32_t>(std::floor(std::log2(std::max(desc.width, desc.height)))) + 1;

							auto tex = rhi_ptr->create_texture(desc, (const void*)img_ptr->pixels,
								(uint64_t)img_ptr->width * img_ptr->height * 4);
							rhi_ptr->set_debug_name(tex, ObjectType::Texture, tex_path);
							rhi_ptr->update_bindless_texture(current_slot, tex);

							//bud::print("[Renderer] ✓ Texture BOUND: {} -> Slot {}", tex_path, current_slot);
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
				RenderMesh new_mesh;

				new_mesh.aabb = cpu_aabb;
				new_mesh.sphere.center = (cpu_aabb.min + cpu_aabb.max) * 0.5f;
				new_mesh.sphere.radius = bud::math::distance(cpu_aabb.max, new_mesh.sphere.center);
				new_mesh.index_count = (uint32_t)mesh_data_copy->indices.size();

				uint64_t v_size = mesh_data_copy->vertices.size() * sizeof(bud::io::MeshData::Vertex);
				uint64_t i_size = mesh_data_copy->indices.size() * sizeof(uint32_t);

				new_mesh.vertex_buffer = rhi->create_gpu_buffer(v_size, ResourceState::VertexBuffer);
				new_mesh.index_buffer = rhi->create_gpu_buffer(i_size, ResourceState::IndexBuffer);
				bud::print("[Renderer::upload_mesh] mesh={} vbuf_valid={} ibuf_valid={} vbuf_state={} ibuf_state={}",
					assigned_mesh_id,
					new_mesh.vertex_buffer.is_valid(),
					new_mesh.index_buffer.is_valid(),
					(void*)new_mesh.vertex_buffer.internal_state,
					(void*)new_mesh.index_buffer.internal_state);

				auto v_stage = rhi->create_upload_buffer(v_size);
				auto i_stage = rhi->create_upload_buffer(i_size);

				std::memcpy(v_stage.mapped_ptr, mesh_data_copy->vertices.data(), v_size);
				std::memcpy(i_stage.mapped_ptr, mesh_data_copy->indices.data(), i_size);
				bud::print("[Renderer::upload_mesh] mesh={} before copy v_stage_valid={} i_stage_valid={} vbuf_valid={} ibuf_valid={}",
					assigned_mesh_id,
					v_stage.is_valid(),
					i_stage.is_valid(),
					new_mesh.vertex_buffer.is_valid(),
					new_mesh.index_buffer.is_valid());

				rhi->copy_buffer_immediate(v_stage, new_mesh.vertex_buffer, v_size);
				rhi->copy_buffer_immediate(i_stage, new_mesh.index_buffer, i_size);

				rhi->destroy_buffer(v_stage);
				rhi->destroy_buffer(i_stage);

				// GPU-Driven Meshlet data upload
				if (!mesh_data_copy->meshlets.empty()) {
					new_mesh.meshlet_count = (uint32_t)mesh_data_copy->meshlets.size();
					uint64_t m_size = mesh_data_copy->meshlets.size() * sizeof(asset::MeshletDescriptor);
					uint64_t mv_size = mesh_data_copy->meshlet_vertices.size() * sizeof(uint32_t);
					uint64_t mt_size = mesh_data_copy->meshlet_triangles.size() * sizeof(uint32_t); // triangles are packed uint32
					uint64_t mc_size = mesh_data_copy->meshlet_cull_data.size() * sizeof(asset::MeshletCullData);

					new_mesh.meshlet_buffer = rhi->create_gpu_buffer(m_size, ResourceState::ShaderResource); // Read in compute
					new_mesh.vertex_index_buffer = rhi->create_gpu_buffer(mv_size, ResourceState::ShaderResource);
					new_mesh.meshlet_index_buffer = rhi->create_gpu_buffer(mt_size, ResourceState::ShaderResource);
					new_mesh.cull_data_buffer = rhi->create_gpu_buffer(mc_size, ResourceState::ShaderResource);

					auto m_stage = rhi->create_upload_buffer(m_size);
					auto mv_stage = rhi->create_upload_buffer(mv_size);
					auto mt_stage = rhi->create_upload_buffer(mt_size);
					auto mc_stage = rhi->create_upload_buffer(mc_size);

					std::memcpy(m_stage.mapped_ptr, mesh_data_copy->meshlets.data(), m_size);
					std::memcpy(mv_stage.mapped_ptr, mesh_data_copy->meshlet_vertices.data(), mv_size);
					std::memcpy(mt_stage.mapped_ptr, mesh_data_copy->meshlet_triangles.data(), mt_size);
					std::memcpy(mc_stage.mapped_ptr, mesh_data_copy->meshlet_cull_data.data(), mc_size);

					rhi->copy_buffer_immediate(m_stage, new_mesh.meshlet_buffer, m_size);
					rhi->copy_buffer_immediate(mv_stage, new_mesh.vertex_index_buffer, mv_size);
					rhi->copy_buffer_immediate(mt_stage, new_mesh.meshlet_index_buffer, mt_size);
					rhi->copy_buffer_immediate(mc_stage, new_mesh.cull_data_buffer, mc_size);

					rhi->destroy_buffer(m_stage);
					rhi->destroy_buffer(mv_stage);
					rhi->destroy_buffer(mt_stage);
					rhi->destroy_buffer(mc_stage);
				}

				if (!mesh_data_copy->subsets.empty()) {
					//bud::print("[upload_mesh] Mesh[{}]: Processing {} subsets", assigned_mesh_id, mesh_data_copy->subsets.size());

					for (size_t i = 0; i < mesh_data_copy->subsets.size(); ++i) {
						const auto& subset = mesh_data_copy->subsets[i];
						SubMesh sub;
						sub.index_start = subset.index_start;
						sub.index_count = subset.index_count;
						sub.meshlet_start = subset.meshlet_start;
						sub.meshlet_count = subset.meshlet_count;

						if (subset.material_index < texture_slot_map.size()) {
							sub.material_id = texture_slot_map[subset.material_index];
							//bud::print("  Subset[{}]: mat_idx={} -> GPU_slot={}", i, subset.material_index, sub.material_id);
						}
						else {
							bud::eprint("  Subset[{}]: INVALID mat_idx={} (max: {}) -> using fallback!",
								i, subset.material_index, texture_slot_map.size());
							sub.material_id = 0;
						}

						sub.aabb = subset.aabb;
						sub.sphere.center = subset.aabb.center();
						sub.sphere.radius = bud::math::distance(subset.aabb.max, sub.sphere.center);

						new_mesh.submeshes.push_back(sub);
					}
				}
				else {
					bud::eprint("[upload_mesh] Mesh[{}]: NO SUBSETS! Using fallback",
						assigned_mesh_id);
					SubMesh sub;
					sub.index_start = 0;
					sub.index_count = (uint32_t)mesh_data_copy->indices.size();
					sub.meshlet_start = 0;
					sub.meshlet_count = new_mesh.meshlet_count;
					sub.material_id = texture_slot_map.empty() ? 0 : texture_slot_map[0];
					new_mesh.submeshes.push_back(sub);
				}

				meshes.push_back(std::move(new_mesh));

				bud::print("[Renderer] Mesh uploaded. Count: {}", meshes.size());
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

	void Renderer::update_ui_draw_data(ImDrawData* draw_data) {
		if (ui_pass) {
			ui_pass->update_draw_data(draw_data);
		}
	}



	void Renderer::render(const bud::graphics::RenderScene& render_scene, SceneView& scene_view) {
		// 先处理所有挂起的上传任务
		flush_upload_queue();

		// 重置当前帧统计数据
		rhi->get_render_stats() = {};

		size_t instance_count = render_scene.instance_count.load(std::memory_order_relaxed);
		const uint32_t cascade_count = std::min(render_config.cascade_count, (uint32_t)MAX_CASCADES);
		uint32_t total_shadow_casters = 0;
		uint32_t total_shadow_caster_submeshes = 0;
		size_t visible_count = 0;
		size_t visible_instance_count = 0;
		size_t total_draw_count = 0;
		std::vector<std::vector<uint32_t>> culled_results(1 + cascade_count);

		if (instance_count > 0) {
			update_cascades(scene_view, render_config, render_scene.scene_bounds);

			std::vector<bud::math::Frustum> view_frustums(1 + cascade_count);
			view_frustums[0].update(scene_view.view_proj_matrix);

			auto& main_visible_instances = culled_results[0];
			main_visible_instances.clear();
			render_scene.cull_frustum(view_frustums[0], main_visible_instances);

			if (cascade_count == 0) {
				total_shadow_casters = static_cast<uint32_t>(main_visible_instances.size());
			}

			if (!main_visible_instances.empty() && cascade_count > 0) {
				for (uint32_t i = 0; i < cascade_count; ++i) {
					view_frustums[i + 1].update(scene_view.cascade_view_proj_matrices[i]);
				}

				bud::threading::Counter culling_counter;

				task_scheduler->ParallelFor(cascade_count, 1,
					[&](size_t start, size_t end) {
						for (size_t cascade_idx = start; cascade_idx < end; ++cascade_idx) {
							auto result_index = cascade_idx + 1;
							auto& visible_instances = culled_results[result_index];
							visible_instances.clear();
							render_scene.cull_frustum(view_frustums[result_index], visible_instances);
							
							// Count submesh-level shadow casters
							for (uint32_t instance : visible_instances) {
								uint32_t mesh_id = render_scene.mesh_indices[instance];
								const auto& mesh = meshes[mesh_id];
								total_shadow_caster_submeshes += static_cast<uint32_t>(mesh.submeshes.size());
							}
						}
					},
					&culling_counter
				);

				task_scheduler->wait_for_counter(culling_counter);

				for (uint32_t v = 1; v <= cascade_count; ++v) {
					total_shadow_casters += (uint32_t)culled_results[v].size();
				}
			}

			const bud::math::Frustum& main_camera_frustum = view_frustums[0];

			const auto& visible_instances = culled_results[0];
			size_t visible_count_initial = visible_instances.size();
			visible_instance_count = visible_count_initial;
			total_draw_count = 0;
			std::vector<uint32_t> draw_offsets(visible_count_initial + 1);
			for (size_t k = 0; k < visible_count_initial; ++k) {
				uint32_t i = visible_instances[k];
				uint32_t mesh_id = render_scene.mesh_indices[i];
				const auto& mesh = meshes[mesh_id];
				uint32_t sub_idx = render_scene.submesh_indices[i];

				draw_offsets[k] = (uint32_t)total_draw_count;
				if (sub_idx == bud::asset::INVALID_INDEX) {
					total_draw_count += (uint32_t)mesh.submeshes.size();
				} else {
					total_draw_count += 1;
				}
			}
			draw_offsets[visible_count_initial] = (uint32_t)total_draw_count;

			if (sort_list.size() < total_draw_count)
				sort_list.resize(total_draw_count);

			if (total_draw_count > 0) {
				SortItem invalid_item{};
				invalid_item.key = UINT64_MAX;
				invalid_item.entity_index = UINT32_MAX;
				invalid_item.submesh_index = UINT32_MAX;
				std::fill(sort_list.begin(), sort_list.begin() + total_draw_count, invalid_item);
			}

			bud::threading::Counter key_gen_signal;
			constexpr size_t KEY_GEN_CHUNK_SIZE = 256;

			task_scheduler->ParallelFor(visible_count_initial, KEY_GEN_CHUNK_SIZE,
				[&](size_t start_exclusive, size_t end_exclusive) {
					for (size_t k = start_exclusive; k < end_exclusive; ++k) {
						uint32_t i = visible_instances[k];
						uint32_t draw_start = draw_offsets[k];
						uint32_t draw_end = draw_offsets[k + 1];
						uint32_t draw_count = draw_end - draw_start;

						if (draw_count == 0)
							continue;

						const auto& world_matrix = render_scene.world_matrices[i];
						uint32_t mesh_id = render_scene.mesh_indices[i];
						const auto& mesh = meshes[mesh_id];
						uint32_t sub_idx_original = render_scene.submesh_indices[i];

						auto mesh_pos = bud::math::vec3(world_matrix[3]);
						auto distance = bud::math::distance2(mesh_pos, scene_view.camera_position);

						uint32_t depth_key = 0;
						auto depth_normalized = std::clamp(distance / (scene_view.far_plane * scene_view.far_plane), 0.0f, 1.0f);
						depth_key = static_cast<uint32_t>(depth_normalized * 0x3FFFF);

						if (sub_idx_original != bud::asset::INVALID_INDEX) {
							auto& item = sort_list[draw_start];
							item.entity_index = (uint32_t)i;
							item.submesh_index = sub_idx_original;
							
							if (sub_idx_original < mesh.submeshes.size()) {
								const auto& sub = mesh.submeshes[sub_idx_original];
								auto world_sub_aabb = sub.aabb.transform(world_matrix);
								if (!bud::math::intersect_aabb_frustum(world_sub_aabb, main_camera_frustum)) {
									item.key = UINT64_MAX;
									continue;
								}
								item.key = DrawKey::generate_opaque(0, 0, sub.material_id, mesh_id, depth_key);
							} else {
								uint32_t material_id = render_scene.material_indices[i];
								item.key = DrawKey::generate_opaque(0, 0, material_id, mesh_id, depth_key);
							}
						} else {
							// Explode!
							for (uint32_t s = 0; s < (uint32_t)mesh.submeshes.size(); ++s) {
								auto& item = sort_list[draw_start + s];
								const auto& sub = mesh.submeshes[s];
								auto world_sub_aabb = sub.aabb.transform(world_matrix);
								if (!bud::math::intersect_aabb_frustum(world_sub_aabb, main_camera_frustum)) {
									item.key = UINT64_MAX;
									item.entity_index = (uint32_t)i;
									item.submesh_index = s;
									continue;
								}

								item.entity_index = (uint32_t)i;
								item.submesh_index = s;
								item.key = DrawKey::generate_opaque(0, 0, sub.material_id, mesh_id, depth_key);
							}
						}
					}
				},
				&key_gen_signal
			);

			task_scheduler->wait_for_counter(key_gen_signal);

			std::sort(sort_list.begin(), sort_list.begin() + total_draw_count,
				[](const SortItem& a, const SortItem& b) { return a.key < b.key; }
			);

			auto it = std::lower_bound(sort_list.begin(), sort_list.begin() + total_draw_count, UINT64_MAX,
				[](const SortItem& item, uint64_t val) { return item.key < val; }
			);

			visible_count = std::distance(sort_list.begin(), it);
		}

		auto cmd = rhi->begin_frame();
		if (!cmd) {
			render_graph.reset(); // Release any transient textures acquired during this frame's setup
			return;
		}

		rhi->set_render_config(render_config);

		auto swapchain_tex = rhi->get_current_swapchain_texture();
		auto back_buffer = render_graph.import_texture("Backbuffer", swapchain_tex, ResourceState::RenderTarget);
		
		uint32_t current_idx = rhi->get_current_image_index();
		RGHandle rg_draw;
		RGHandle rg_inst;
		RGHandle rg_stats;

		struct DrawData {
			uint32_t indexCount;
			uint32_t firstIndex;
			uint32_t materialId;
			uint32_t meshId;
			bud::math::vec3 min;
			uint32_t padding0;
			bud::math::vec3 max;
			uint32_t padding1;
			uint32_t meshletCount;
			uint32_t padding2[3];
		};

		if (instance_count > 0) {
			if (render_config.enable_gpu_driven) {
				if (indirect_instance_buffers.size() <= current_idx) {
					indirect_instance_buffers.resize(current_idx + 1);
					indirect_draw_buffers.resize(current_idx + 1);
					stats_readback_buffers.resize(current_idx + 1);
				}

				if (total_draw_count > current_indirect_capacity) {
					rhi->wait_idle();
					for (auto& buf : indirect_instance_buffers)
						if (buf.is_valid())
							rhi->destroy_buffer(buf);

					for (auto& buf : indirect_draw_buffers)
						if (buf.is_valid())
							rhi->destroy_buffer(buf);

					for (auto& buf : stats_readback_buffers)
						if (buf.is_valid())
							rhi->destroy_buffer(buf);

					// Growth strategy: double current capacity, but at least (needed + headroom).
					constexpr uint32_t kCapacityHeadroom = 1024;
					current_indirect_capacity = std::max(current_indirect_capacity * 2, static_cast<uint32_t>(total_draw_count) + kCapacityHeadroom);

					
					for (size_t i = 0; i < indirect_instance_buffers.size(); ++i) {
						indirect_instance_buffers[i] = rhi->create_gpu_buffer(current_indirect_capacity * sizeof(DrawData), ResourceState::UnorderedAccess);
						indirect_draw_buffers[i] = rhi->create_gpu_buffer(current_indirect_capacity * sizeof(IndirectCommand), ResourceState::IndirectArgument); 
						
						stats_readback_buffers[i] = rhi->create_gpu_buffer(1024, ResourceState::UnorderedAccess); // Extra space for alignment/future stats
						
						rhi->set_debug_name(indirect_instance_buffers[i], ObjectType::Buffer, "IndirectInstanceData_Frame" + std::to_string(i));
						rhi->set_debug_name(indirect_draw_buffers[i], ObjectType::Buffer, "IndirectDrawCommands_Frame" + std::to_string(i));
						rhi->set_debug_name(stats_readback_buffers[i], ObjectType::Buffer, "GPUStatsReadback_Frame" + std::to_string(i));
					}
				}
					if (visible_count > 0) {
					auto& current_inst_buf = indirect_instance_buffers[current_idx];
					auto& current_draw_buf = indirect_draw_buffers[current_idx];
					auto& current_stats_buf = stats_readback_buffers[current_idx];

					if ((!current_inst_buf.is_valid() || !current_draw_buf.is_valid()) && current_indirect_capacity == 0) {
						current_indirect_capacity = std::max<uint32_t>(static_cast<uint32_t>(total_draw_count) + 1024u, 1024u);
					}

					if (!current_inst_buf.is_valid()) {
						current_inst_buf = rhi->create_gpu_buffer(current_indirect_capacity * sizeof(DrawData), ResourceState::UnorderedAccess);
						rhi->set_debug_name(current_inst_buf, ObjectType::Buffer, "IndirectInstanceData_Frame" + std::to_string(current_idx));
					}
					if (!current_draw_buf.is_valid()) {
						current_draw_buf = rhi->create_gpu_buffer(current_indirect_capacity * sizeof(IndirectCommand), ResourceState::IndirectArgument);
						rhi->set_debug_name(current_draw_buf, ObjectType::Buffer, "IndirectDrawCommands_Frame" + std::to_string(current_idx));
					}
					if (!current_stats_buf.is_valid()) {
						current_stats_buf = rhi->create_gpu_buffer(1024, ResourceState::UnorderedAccess);
						rhi->set_debug_name(current_stats_buf, ObjectType::Buffer, "GPUStatsReadback_Frame" + std::to_string(current_idx));
					}

						auto staging = rhi->create_upload_buffer(visible_count * sizeof(DrawData));
					DrawData* mapped = static_cast<DrawData*>(staging.mapped_ptr);
						for (size_t i = 0; i < visible_count; ++i) {
						const auto& item = sort_list[i];
						uint32_t entity_idx = item.entity_index;
						uint32_t mesh_id = render_scene.mesh_indices[entity_idx];
						const auto& mesh = meshes[mesh_id];

						if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[item.submesh_index];
							mapped[i].indexCount = sub.index_count;
							mapped[i].firstIndex = sub.index_start;
							mapped[i].materialId = sub.material_id;
							mapped[i].meshId = mesh_id;
							
							auto world_aabb = sub.aabb.transform(render_scene.world_matrices[entity_idx]);
							mapped[i].min = world_aabb.min;
							mapped[i].max = world_aabb.max;
							mapped[i].meshletCount = sub.meshlet_count;
						} else {
							mapped[i].indexCount = mesh.index_count;
							mapped[i].firstIndex = 0;
							mapped[i].materialId = render_scene.material_indices[entity_idx];
							mapped[i].meshId = mesh_id;

							auto world_aabb = mesh.aabb.transform(render_scene.world_matrices[entity_idx]);
							mapped[i].min = world_aabb.min;
							mapped[i].max = world_aabb.max;
							mapped[i].meshletCount = mesh.meshlet_count;
						}
					}
						rhi->copy_buffer_immediate(staging, current_inst_buf, visible_count * sizeof(DrawData));
					rhi->destroy_buffer(staging);

					rg_inst = render_graph.import_buffer("IndirectInstanceData", current_inst_buf, ResourceState::UnorderedAccess);
					rg_draw = render_graph.import_buffer("IndirectDrawCommands", current_draw_buf, ResourceState::IndirectArgument);
					rg_stats = render_graph.import_buffer("GPUStatsReadback", stats_readback_buffers[current_idx], ResourceState::UnorderedAccess);
				}

				// Read back previous frame stats (delayed latency) from this exact buffer which is guaranteed finished
				if (stats_readback_buffers[current_idx].is_valid()) {
					GPUStats* gpu_stats = static_cast<GPUStats*>(stats_readback_buffers[current_idx].mapped_ptr);
					if (gpu_stats) {
						last_gpu_stats = *gpu_stats;
					}
				}
			}

			// Calculate CPU Frustum Culling Stats
			uint32_t scene_total_objs = 0;
			uint32_t scene_total_tris = 0;
			
			for (size_t i = 0; i < render_scene.size(); ++i) {
				uint32_t mesh_id = render_scene.mesh_indices[i];
				if (mesh_id >= meshes.size() || !meshes[mesh_id].is_valid()) continue;
				
				if (meshes[mesh_id].submeshes.empty()) {
					scene_total_objs += 1;
					scene_total_tris += meshes[mesh_id].index_count / 3;
				} else {
					scene_total_objs += 1;
					if (render_scene.submesh_indices[i] != bud::asset::INVALID_INDEX && render_scene.submesh_indices[i] < meshes[mesh_id].submeshes.size()) {
						scene_total_tris += meshes[mesh_id].submeshes[render_scene.submesh_indices[i]].index_count / 3;
					} else {
						for (const auto& sub : meshes[mesh_id].submeshes) {
							scene_total_tris += sub.index_count / 3;
						}
					}
				}
			}

			uint32_t cpu_total_instances = static_cast<uint32_t>(total_draw_count);
			uint32_t cpu_visible_instances = static_cast<uint32_t>(visible_count);
			uint32_t cpu_total_meshlets = 0;
			uint32_t cpu_visible_meshlets = 0;

			uint32_t cpu_total_tris = 0;
			uint32_t cpu_visible_tris = 0;
			for (size_t i = 0; i < total_draw_count; ++i) {
				const auto& item = sort_list[i];
				if (item.entity_index == UINT32_MAX && item.key == UINT64_MAX) continue;
				uint32_t mesh_id = render_scene.mesh_indices[item.entity_index];
				if (mesh_id >= meshes.size()) continue;
				
				uint32_t tris = 0;
				uint32_t meshlets = 0;
				if (item.submesh_index != UINT32_MAX && item.submesh_index < meshes[mesh_id].submeshes.size()) {
					const auto& sub = meshes[mesh_id].submeshes[item.submesh_index];
					tris = sub.index_count / 3;
					meshlets = sub.meshlet_count;
				} else {
					tris = meshes[mesh_id].index_count / 3;
					meshlets = meshes[mesh_id].meshlet_count;
				}
				cpu_total_tris += tris;
				cpu_total_meshlets += meshlets;
				if (i < visible_count) {
					cpu_visible_tris += tris;
					cpu_visible_meshlets += meshlets;
				}
			}

			// Setup GPU Stats
			if (render_config.enable_gpu_driven) {
				rhi->add_culling_stats(scene_total_objs, (uint32_t)visible_instance_count, total_shadow_casters);
				if (visible_count > 0) {
					rhi->get_render_stats().gpu_total_instances = last_gpu_stats.totalInstances;
					rhi->get_render_stats().gpu_visible_instances = last_gpu_stats.visibleInstances;
					rhi->get_render_stats().gpu_total_triangles = last_gpu_stats.totalTriangles;
					rhi->get_render_stats().gpu_visible_triangles = last_gpu_stats.visibleTriangles;
					rhi->get_render_stats().gpu_total_meshlets = last_gpu_stats.totalMeshlets;
					rhi->get_render_stats().gpu_visible_meshlets = last_gpu_stats.visibleMeshlets;
				} else {
					last_gpu_stats = {};
					rhi->get_render_stats().gpu_total_instances = 0;
					rhi->get_render_stats().gpu_visible_instances = 0;
					rhi->get_render_stats().gpu_total_triangles = 0;
					rhi->get_render_stats().gpu_visible_triangles = 0;
					rhi->get_render_stats().gpu_total_meshlets = 0;
					rhi->get_render_stats().gpu_visible_meshlets = 0;
				}
			} else {
				rhi->add_culling_stats(scene_total_objs, (uint32_t)visible_instance_count, total_shadow_casters);
				rhi->get_render_stats().gpu_total_instances = cpu_visible_instances;
				rhi->get_render_stats().gpu_visible_instances = cpu_visible_instances;
				rhi->get_render_stats().gpu_total_triangles = cpu_visible_tris; // CPU fallback for GPU
				rhi->get_render_stats().gpu_visible_triangles = cpu_visible_tris;
				rhi->get_render_stats().gpu_total_meshlets = cpu_visible_meshlets;
				rhi->get_render_stats().gpu_visible_meshlets = cpu_visible_meshlets;
			}
			
			// Push CPU Stats
			rhi->get_render_stats().cpu_total_objects = scene_total_objs;
			rhi->get_render_stats().cpu_visible_objects = (uint32_t)visible_instance_count;
			rhi->get_render_stats().cpu_total_instances = cpu_total_instances;
			rhi->get_render_stats().cpu_visible_instances = cpu_visible_instances;
			rhi->get_render_stats().cpu_total_triangles = scene_total_tris;
			rhi->get_render_stats().cpu_visible_triangles = cpu_visible_tris;
			rhi->get_render_stats().cpu_total_meshlets = cpu_total_meshlets;
			rhi->get_render_stats().cpu_visible_meshlets = cpu_visible_meshlets;

			// Push shadow caster stats to RenderStats
			rhi->get_render_stats().shadow_casters = total_shadow_casters;
			rhi->get_render_stats().shadow_caster_submeshes = total_shadow_caster_submeshes;

			bool has_main_pass = false;
			if (visible_count > 0) {
				// Z-Prepass ALWAYS uses CPU frustum-culling (visible_count) regardless of GPU-driven settings,
				// as it must generate the depth buffer for Hi-Z culling itself.
				auto depth_prepass = z_prepass->add_to_graph(render_graph, back_buffer, render_scene, scene_view, render_config, meshes, sort_list, visible_count, RGHandle{});
				
				if (depth_prepass.is_valid()) {
					if (render_config.enable_gpu_driven) {
						auto rg_hiz = hiz_mip_pass->add_to_graph(render_graph, depth_prepass, render_config);
						hiz_pass->add_to_graph(render_graph, rg_inst, rg_draw, rg_stats, rg_hiz, scene_view, (uint32_t)visible_count);

						if (render_config.debug_hiz) {
							hiz_debug_pass->add_to_graph(render_graph, back_buffer, rg_hiz, render_config.debug_hiz_mip);
						}
					}

					std::vector<std::vector<uint32_t>> csm_visible_instances(cascade_count);
					for (uint32_t i = 0; i < cascade_count; ++i) csm_visible_instances[i] = std::move(culled_results[i + 1]);

					auto shadow_map = csm_pass->add_to_graph(render_graph, scene_view, render_config, render_scene, meshes, std::move(csm_visible_instances));
					if (shadow_map.is_valid()) {
						main_pass->add_to_graph(render_graph, shadow_map, back_buffer, depth_prepass, render_scene, scene_view, render_config, meshes, sort_list, visible_count, rg_draw);
						has_main_pass = true;
					}
				}
			}

			if (!has_main_pass) {
				render_graph.add_pass("UI Clear Pass",
					[=](RGBuilder& builder) { builder.write(back_buffer, ResourceState::RenderTarget); },
					[this, back_buffer](RHI* rhi, CommandHandle cmd) {
						RenderPassBeginInfo info;
						info.color_attachments.push_back(render_graph.get_texture(back_buffer));
						info.clear_color = true;
						info.clear_color_value = { 0.5f, 0.5f, 0.5f, 1.0f };
						rhi->cmd_begin_render_pass(cmd, info);
						rhi->cmd_end_render_pass(cmd);
					}
				);
			}
		} else {
			render_graph.add_pass("Empty Scene Clear",
				[=](RGBuilder& builder) { builder.write(back_buffer, ResourceState::RenderTarget); },
				[this, back_buffer](RHI* rhi, CommandHandle cmd) {
					RenderPassBeginInfo info;
					info.color_attachments.push_back(render_graph.get_texture(back_buffer));
					info.clear_color = true;
					info.clear_color_value = { 0.2f, 0.2f, 0.2f, 1.0f };
				 rhi->cmd_begin_render_pass(cmd, info);
					rhi->cmd_end_render_pass(cmd);
				}
			);
		}

		ui_pass->add_to_graph(render_graph, back_buffer);
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
