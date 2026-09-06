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
#include "src/streaming/bud.streaming.manager.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/runtime/bud.scene.hpp"
#include "src/core/bud.layouts.hpp"

#include "src/graphics/bud.graphics.sortkey.hpp"
#include "src/graphics/vulkan/bud.vulkan.memory.hpp"



namespace bud::graphics {
namespace {
struct LayoutValidation {
    static_assert(sizeof(Renderer::HierarchyInstance) == 112,
        "HierarchyInstance size must match GLSL layout (112 bytes)");
    static_assert(offsetof(Renderer::HierarchyInstance, model_matrix) == 0, "");
    static_assert(offsetof(Renderer::HierarchyInstance, mesh_id) == 64, "");
    static_assert(offsetof(Renderer::HierarchyInstance, material_id) == 68, "");
    static_assert(offsetof(Renderer::HierarchyInstance, root_group_index) == 72, "");
    static_assert(offsetof(Renderer::HierarchyInstance, flags) == 76, "");
    static_assert(offsetof(Renderer::HierarchyInstance, global_sphere_center) == 80, "");
    static_assert(offsetof(Renderer::HierarchyInstance, global_sphere_radius) == 92, "");
    static_assert(offsetof(Renderer::HierarchyInstance, error_threshold) == 96, "");
    static_assert(offsetof(Renderer::HierarchyInstance, base_virtual_page) == 100, "");
    static_assert(offsetof(Renderer::HierarchyInstance, padding) == 104, "");

    static_assert(sizeof(Renderer::InstanceData) == 80,
        "InstanceData size must match GLSL layout (80 bytes)");
    static_assert(offsetof(Renderer::InstanceData, model) == 0, "");
    static_assert(offsetof(Renderer::InstanceData, material_id) == 64, "");
    static_assert(offsetof(Renderer::InstanceData, page_slot) == 68, "");
    static_assert(offsetof(Renderer::InstanceData, blend_factor) == 72, "");
    static_assert(offsetof(Renderer::InstanceData, padding) == 76, "");
};
}
}

namespace bud::graphics {

	Renderer::Renderer(RHI* rhi, bud::io::AssetManager* asset_manager, bud::threading::TaskScheduler* task_scheduler)
		: rhi(rhi), render_graph(rhi), asset_manager(asset_manager), task_scheduler(task_scheduler) {
		upload_queue = std::make_shared<UploadQueue>();
		csm_pass = std::make_unique<CSMShadowPass>();
		depth_only_pass = std::make_unique<DepthOnlyPass>();
		ao_pass = std::make_unique<AmbientOcclusionPass>();
		ao_temporal_pass = std::make_unique<AOTemporalPass>();
		ao_blur_pass = std::make_unique<AOBlurPass>();
		pyramid_mip_pass = std::make_unique<PyramidMipPass>();
		instance_culling_pass = std::make_unique<InstanceCullingPass>();
		pyramid_mip_debug_pass = std::make_unique<PyramidMipDebugPass>();
		hierarchy_traversal_pass = std::make_unique<HierarchyTraversalPass>();
		page_emit_pass = std::make_unique<PageEmitPass>();
		cluster_cull_pass = std::make_unique<ClusterCullPass>();
		forward_translucent_pass = std::make_unique<ForwardTranslucentPass>();
		ui_pass = std::make_unique<UIPass>();
		visibility_pass = std::make_unique<VisibilityPass>();
		ssr_pass = std::make_unique<ScreenSpaceReflectionPass>();
		ssgi_pass = std::make_unique<ScreenSpaceGlobalIlluminationPass>();
		resolve_pass = std::make_unique<ResolvePass>();

		csm_pass->init(rhi, render_config, asset_manager);
		depth_only_pass->init(rhi, render_config, asset_manager);
		ao_pass->init(rhi, render_config, asset_manager);
		ao_temporal_pass->init(rhi, render_config, asset_manager);
		ao_blur_pass->init(rhi, render_config, asset_manager);
		pyramid_mip_pass->init(rhi, render_config, asset_manager);
		instance_culling_pass->init(rhi, render_config, asset_manager);
		pyramid_mip_debug_pass->init(rhi, render_config, asset_manager);
		hierarchy_traversal_pass->init(rhi, render_config, asset_manager);
		page_emit_pass->init(rhi, render_config, asset_manager);
		cluster_cull_pass->init(rhi, render_config, asset_manager);
		forward_translucent_pass->init(rhi, render_config, asset_manager);
		ui_pass->init(rhi, render_config, asset_manager);
		visibility_pass->init(rhi, render_config, asset_manager);
		ssr_pass->init(rhi, render_config, asset_manager);
		ssgi_pass->init(rhi, render_config, asset_manager);
		resolve_pass->init(rhi, render_config, asset_manager);
		physics_debug_pass = std::make_unique<PhysicsDebugPass>();
		physics_debug_pass->init(rhi, render_config, asset_manager);

		auto& geometry_pool = gpu_scene.get_geometry_pool();
		if (!geometry_pool.initialized) {
			geometry_pool.vertex_buffer = rhi->create_gpu_buffer(GPUScene::GeometryPool::vertex_pool_size, ResourceState::VertexBuffer);
			geometry_pool.index_buffer = rhi->create_gpu_buffer(GPUScene::GeometryPool::index_pool_size, ResourceState::IndexBuffer);
			rhi->set_debug_name(geometry_pool.vertex_buffer, ObjectType::Buffer, "GeometryPool_Vertices");
			rhi->set_debug_name(geometry_pool.index_buffer, ObjectType::Buffer, "GeometryPool_Indices");
			geometry_pool.initialized = true;
			bud::print("[GeometryPool] Initialized: vertex={}MB index={}MB",
				GPUScene::GeometryPool::vertex_pool_size / (1024 * 1024),
				GPUScene::GeometryPool::index_pool_size / (1024 * 1024));
		}

		has_mesh_shader = true;
		gpu_scene.init(rhi, rhi->get_inflight_frame_count());

		// Load CSM cull shader for GPU-driven shadow culling
		asset_manager->load_file_async("src/shaders/csm_cull.comp.spv", [this, rhi](std::vector<char> data) {
			if (data.empty()) {
				bud::eprint("[Renderer] Failed to load CSM cull shader");
				return;
			}
			ComputePipelineDesc desc;
			desc.layout_kind = ComputePipelineDesc::LayoutKind::CSMCulling;
			desc.cs.code = data;
			csm_cull_pipeline = rhi->create_compute_pipeline(desc);
			if (csm_cull_pipeline.is_valid())
				bud::print("[Renderer] CSM cull shader loaded and pipeline created.");
		});
	}

	Renderer::~Renderer() {
		flush_upload_queue();
		upload_queue.reset();

		if (csm_pass) csm_pass->shutdown(rhi);
		if (depth_only_pass) depth_only_pass->shutdown(rhi);
		if (ao_pass) ao_pass->shutdown(rhi);
		if (ao_temporal_pass) ao_temporal_pass->shutdown(rhi);
		if (ao_blur_pass) ao_blur_pass->shutdown(rhi);
		if (pyramid_mip_pass) pyramid_mip_pass->shutdown(rhi);
		if (instance_culling_pass) instance_culling_pass->shutdown(rhi);
		if (pyramid_mip_debug_pass) pyramid_mip_debug_pass->shutdown(rhi);
		if (hierarchy_traversal_pass) hierarchy_traversal_pass->shutdown(rhi);
		if (page_emit_pass) page_emit_pass->shutdown(rhi);
		if (cluster_cull_pass) cluster_cull_pass->shutdown(rhi);
		if (forward_translucent_pass) forward_translucent_pass->shutdown(rhi);
		if (visibility_pass) visibility_pass->shutdown(rhi);
		if (ssr_pass) ssr_pass->shutdown(rhi);
		if (ssgi_pass) ssgi_pass->shutdown(rhi);
		if (resolve_pass) resolve_pass->shutdown(rhi);
		if (physics_debug_pass) physics_debug_pass->shutdown(rhi);
		if (csm_cull_pipeline.is_valid()) {
			rhi->destroy_pipeline(csm_cull_pipeline);
			csm_cull_pipeline.reset();
		}
		if (ui_pass) ui_pass->shutdown(rhi);
		gpu_scene.shutdown(rhi);


		for (auto& buf : readback_buffers) {
			if (buf.is_valid()) rhi->destroy_buffer(buf);
		}

		if (offscreen_target.is_valid()) {
			rhi->destroy_texture(offscreen_target);
			offscreen_target.reset();
		}

	}

	std::vector<bud::math::AABB> Renderer::get_mesh_bounds_snapshot() const {
		std::lock_guard lock(mesh_bounds_mutex);
		return mesh_bounds;
	}

	std::vector<std::vector<bud::math::AABB>> Renderer::get_submesh_bounds_snapshot() const {
		std::scoped_lock lock(mesh_mutex, mesh_bounds_mutex);
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

	void Renderer::register_mesh_bounds(uint32_t mesh_id, const bud::math::AABB& aabb) {
		std::lock_guard lock(mesh_bounds_mutex);
		if (mesh_bounds.size() <= mesh_id) {
			mesh_bounds.resize(mesh_id + 1);
		}
		mesh_bounds[mesh_id] = aabb;

		std::lock_guard mesh_lock(mesh_mutex);
		if (meshes.size() <= mesh_id) {
			meshes.resize(mesh_id + 1);
		}
		meshes[mesh_id].aabb = aabb;
		meshes[mesh_id].sphere.center = (aabb.min + aabb.max) * 0.5f;
		meshes[mesh_id].sphere.radius = bud::math::distance(aabb.max, meshes[mesh_id].sphere.center);
		meshes[mesh_id].is_page_based = true;
	}

	uint32_t Renderer::register_page_based_mesh(uint32_t page_index, uint32_t cluster_count,
		uint32_t index_count, const bud::math::AABB& aabb, const bud::math::AABB& global_aabb,
		uint32_t vertex_data_offset, uint32_t index_data_offset,
		const std::vector<PageSubMesh>& page_submeshes,
		const std::vector<std::pair<uint32_t, uint32_t>>& lod_index_ranges,
		const float lod_errors[3])
	{
		std::scoped_lock lock(mesh_mutex, mesh_bounds_mutex);

		uint32_t mesh_id = next_mesh_id.fetch_add(1, std::memory_order_relaxed);

		RenderMesh mesh{};
		mesh.index_count = index_count;
		mesh.is_page_based = true;
		mesh.page_index = page_index;
		mesh.page_vertex_data_offset = vertex_data_offset;
		mesh.page_index_data_offset = index_data_offset;
		mesh.aabb = aabb;
		{
			bud::math::vec3 center = (aabb.min + aabb.max) * 0.5f;
			float radius = bud::math::length(aabb.max - center);
			mesh.sphere = bud::math::BoundingSphere(center, radius);
		}
		{
			bud::math::vec3 global_center = (global_aabb.min + global_aabb.max) * 0.5f;
			float global_radius = bud::math::length(global_aabb.max - global_center);
			mesh.global_sphere = bud::math::BoundingSphere(global_center, global_radius);
		}


		if (lod_errors) {
			for (int i = 0; i < 3; ++i)
				mesh.lod_error[i] = lod_errors[i];
		}
		else {
			mesh.lod_error[0] = 0.0f;
			mesh.lod_error[1] = FLT_MAX;
			mesh.lod_error[2] = FLT_MAX;
		}

		for (size_t i = 0; i < lod_index_ranges.size() && i < 3; ++i) {
			mesh.lod_index_start[i] = lod_index_ranges[i].first;
			mesh.lod_index_count[i] = lod_index_ranges[i].second;
		}

		// Build one render submesh per per-material run inside the page so draw-count
		// accounting, per-submesh culling and material assignment all work for pages.
		if (page_submeshes.empty()) {
			SubMesh fallback_sub{};
			fallback_sub.index_start = 0;
			fallback_sub.index_count = index_count;
			fallback_sub.material_id = 0;
			fallback_sub.aabb = aabb;
			{
				bud::math::vec3 center = (aabb.min + aabb.max) * 0.5f;
				float radius = bud::math::length(aabb.max - center);
				fallback_sub.sphere = bud::math::BoundingSphere(center, radius);
			}
			mesh.submeshes.push_back(fallback_sub);
		}
		else {
			for (size_t i = 0; i < page_submeshes.size(); ++i) {
				const auto& ps = page_submeshes[i];
				SubMesh sub{};
				sub.index_start = ps.index_start;
				sub.index_count = ps.index_count;
	
				sub.material_id = ps.material_id;
				sub.lod_level = ps.lod_level;
				sub.page_index = ps.page_index;
				sub.aabb = ps.aabb;
				bud::math::vec3 center = (ps.aabb.min + ps.aabb.max) * 0.5f;
				float radius = bud::math::length(ps.aabb.max - center);
				sub.sphere = bud::math::BoundingSphere(center, radius);
				mesh.submeshes.push_back(sub);
			}
		}

		if (mesh_id >= meshes.size())
			meshes.resize(mesh_id + 1);
		meshes[mesh_id] = std::move(mesh);

		if (mesh_id >= mesh_bounds.size())
			mesh_bounds.resize(mesh_id + 1);
		mesh_bounds[mesh_id] = aabb;

		gpu_scene.set_mesh_geometry(mesh_id, static_cast<uint32_t>((page_index * GPUScene::PagePool::page_size + index_data_offset) / sizeof(uint16_t)), 0);

		return mesh_id;
	}

	uint32_t Renderer::bind_texture_async(const std::string& path) {
		if (path.empty()) return 0;
		uint32_t current_slot = 0;
		{
			std::lock_guard lock(texture_slot_mutex);
			auto it = bound_texture_slots.find(path);
			if (it != bound_texture_slots.end()) {
				return it->second;
			}
			current_slot = next_bindless_slot.fetch_add(1, std::memory_order_relaxed);
			bound_texture_slots[path] = current_slot;
		}

		auto queue = upload_queue;
		auto queue_weak = std::weak_ptr<UploadQueue>(upload_queue);
		auto rhi_ptr = rhi;

		{
			std::lock_guard lock(queue->mutex);
			queue->commands.push_back([rhi_ptr, current_slot]() {
				rhi_ptr->queue_bindless_fallback(current_slot, rhi_ptr->get_fallback_texture());
			});
		}

		// Kick off async image load and bind to the reserved slot once decoded.
		asset_manager->load_image_async(path,
			[queue_weak, rhi_ptr, current_slot, path](bud::io::Image img) {
				auto img_ptr = std::make_shared<bud::io::Image>(std::move(img));

				auto queue_locked = queue_weak.lock();
				if (!queue_locked) {
					std::string err = "Renderer::bind_texture_async upload queue was destroyed before callback";
					bud::eprint("{}", err);
#if defined(_DEBUG)
					throw std::runtime_error(err);
#else
					return;
#endif
				}

				std::lock_guard lock(queue_locked->mutex);
				queue_locked->commands.push_back([rhi_ptr, current_slot, path, img_ptr]() {
					bud::graphics::TextureDesc desc{};
					desc.width = (uint32_t)img_ptr->width;
					desc.height = (uint32_t)img_ptr->height;
					desc.format = bud::graphics::TextureFormat::RGBA8_SRGB;
					desc.mips = static_cast<uint32_t>(std::log2(std::max(desc.width, desc.height))) + 1u;

					auto tex = rhi_ptr->create_texture_async(desc, (const void*)img_ptr->pixels,
						(uint64_t)img_ptr->width * img_ptr->height * 4, current_slot);
					rhi_ptr->set_debug_name(tex, ObjectType::Texture, path);
				});
			}
		);

		return current_slot;
	}

	MeshAssetHandle Renderer::upload_mesh(const bud::io::MeshData& mesh_data) {
		if (mesh_data.vertices.empty()) {
			std::string err = "Renderer::upload_mesh called with empty vertex list";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return MeshAssetHandle::invalid();
#endif
		}

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
			for (size_t i = 0; i < mesh_data.texture_paths.size(); ++i) {
				const auto& tex_path = mesh_data.texture_paths[i];
				uint32_t current_slot = bind_texture_async(tex_path);
				texture_slot_map.push_back(current_slot);

				if (i == 0) {
					base_material_id = current_slot;
				}
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

				const uint32_t vertex_count = (uint32_t)mesh_data_copy->vertices.size();
				const uint32_t index_count = (uint32_t)mesh_data_copy->indices.size();
				const uint64_t v_size = vertex_count * sizeof(bud::io::MeshData::Vertex);
				const uint64_t i_size = index_count * sizeof(uint32_t);

				// Initialize GPUScene geometry pool on first use
				auto& geometry_pool = gpu_scene.get_geometry_pool();
				if (!geometry_pool.initialized) {
					geometry_pool.vertex_buffer = rhi->create_gpu_buffer(GPUScene::GeometryPool::vertex_pool_size, ResourceState::VertexBuffer);
					geometry_pool.index_buffer = rhi->create_gpu_buffer(GPUScene::GeometryPool::index_pool_size, ResourceState::IndexBuffer);
					rhi->set_debug_name(geometry_pool.vertex_buffer, ObjectType::Buffer, "GeometryPool_Vertices");
					rhi->set_debug_name(geometry_pool.index_buffer, ObjectType::Buffer, "GeometryPool_Indices");
					geometry_pool.initialized = true;
					bud::print("[GeometryPool] Initialized: vertex={}MB index={}MB",
						GPUScene::GeometryPool::vertex_pool_size / (1024 * 1024),
						GPUScene::GeometryPool::index_pool_size / (1024 * 1024));
				}

				// Atomically reserve contiguous region inside the pool
				const uint32_t vertex_base = geometry_pool.next_vertex.fetch_add(vertex_count, std::memory_order_relaxed);
				const uint32_t index_base = geometry_pool.next_index.fetch_add(index_count, std::memory_order_relaxed);

				const uint64_t vertex_pool_byte_offset = (uint64_t)vertex_base * sizeof(bud::io::MeshData::Vertex);
				const uint64_t index_pool_byte_offset = (uint64_t)index_base * sizeof(uint32_t);

				gpu_scene.set_mesh_geometry(assigned_mesh_id, index_base, static_cast<int32_t>(vertex_base));

				// Stage upload: CPU -> staging -> GPU pool
				auto v_stage = rhi->get_allocator()->alloc_staging(v_size);
				auto i_stage = rhi->get_allocator()->alloc_staging(i_size);

				std::memcpy(v_stage.mapped_ptr, mesh_data_copy->vertices.data(), v_size);
				std::memcpy(i_stage.mapped_ptr, mesh_data_copy->indices.data(), i_size);

				rhi->copy_buffer_immediate_offset(v_stage.buffer, geometry_pool.vertex_buffer, v_size, v_stage.offset, vertex_pool_byte_offset);
				rhi->copy_buffer_immediate_offset(i_stage.buffer, geometry_pool.index_buffer, i_size, i_stage.offset, index_pool_byte_offset);

				if (!mesh_data_copy->subsets.empty()) {
					std::vector<uint32_t> material_to_id;
					material_to_id.resize(mesh_data_copy->materials.size(), 0);
					for (size_t mi = 0; mi < mesh_data_copy->materials.size(); ++mi) {
						const auto& mat_data = mesh_data_copy->materials[mi];
						bud::graphics::GPUMaterialData gpu_mat{};
						gpu_mat.alpha_mode = static_cast<uint32_t>(mat_data.alpha_mode);
						gpu_mat.alpha_cutoff = (mat_data.alpha_cutoff > 0.0f) ? mat_data.alpha_cutoff : 0.5f;
						gpu_mat.base_color_factor = mat_data.base_color_factor;
						gpu_mat.metallic_factor = mat_data.metallic_factor;
						gpu_mat.roughness_factor = (mat_data.roughness_factor > 0.0f) ? mat_data.roughness_factor : 0.5f;

						uint32_t tex_idx = mat_data.base_color_texture;
						if (tex_idx < texture_slot_map.size() && texture_slot_map[tex_idx] > 0) {
							gpu_mat.albedo_texture_id = texture_slot_map[tex_idx];
							gpu_mat.base_color_factor = glm::vec4(1.0f);
						}
						else {
							gpu_mat.albedo_texture_id = 0u;
						}

						// Physical Translucent Material Tuning:
						// Support distinct glass, wine bottles, liquor bottles, drinking glasses, and jars
						if (gpu_mat.alpha_mode == 2) {
							std::string path_lower = mesh_data_copy->source_path;
							for (char& c : path_lower) c = static_cast<char>(std::tolower(c));

							if (gpu_mat.albedo_texture_id > 0) {
								// Textured translucent surface (e.g. bottle labels, stickers, curtains)
								gpu_mat.base_color_factor = glm::vec4(1.0f);
								gpu_mat.roughness_factor = 0.05f;
							}
							else if (path_lower.find("liquorbottle") != std::string::npos || path_lower.find("winebottle") != std::string::npos || path_lower.find("bottle") != std::string::npos) {
								// Bottle Glass: distribute rich authentic bottle colors (green, amber, deep bordeaux, clear)
								size_t hash_val = std::hash<std::string>{}(mesh_data_copy->source_path);
								uint32_t variant = static_cast<uint32_t>((hash_val ^ (mi * 1337u)) % 4u);
								if (variant == 0) {
									// Green Wine Bottle (emerald olive green)
									gpu_mat.base_color_factor = glm::vec4(0.12f, 0.42f, 0.18f, 0.45f);
									gpu_mat.roughness_factor = 0.05f;
								}
								else if (variant == 1) {
									// Amber Whiskey / Bourbon Bottle (warm golden amber)
									gpu_mat.base_color_factor = glm::vec4(0.55f, 0.28f, 0.08f, 0.50f);
									gpu_mat.roughness_factor = 0.06f;
								}
								else if (variant == 2) {
									// Deep Bordeaux Bottle (rich dark green-brown)
									gpu_mat.base_color_factor = glm::vec4(0.14f, 0.22f, 0.08f, 0.55f);
									gpu_mat.roughness_factor = 0.06f;
								}
								else {
									// Clear Liquor / Vodka Bottle (clear with subtle refraction tint)
									gpu_mat.base_color_factor = glm::vec4(0.88f, 0.94f, 0.98f, 0.08f);
									gpu_mat.roughness_factor = 0.03f;
								}
							}
							else if (path_lower.find("wineglass") != std::string::npos || path_lower.find("glass") != std::string::npos || path_lower.find("cup") != std::string::npos || path_lower.find("tumbler") != std::string::npos) {
								// Crystal Wine Glass / Water Tumbler (high clarity, low roughness)
								gpu_mat.base_color_factor = glm::vec4(0.96f, 0.98f, 1.0f, 0.04f);
								gpu_mat.roughness_factor = 0.02f;
							}
							else if (path_lower.find("jar") != std::string::npos || path_lower.find("condiment") != std::string::npos || path_lower.find("canister") != std::string::npos) {
								// Molded Glass Jar (faint jade tint, slightly thicker)
								gpu_mat.base_color_factor = glm::vec4(0.82f, 0.94f, 0.88f, 0.20f);
								gpu_mat.roughness_factor = 0.08f;
							}
							else if (path_lower.find("window") != std::string::npos) {
								// Architectural Window Pane
								gpu_mat.base_color_factor = glm::vec4(0.90f, 0.96f, 0.98f, 0.06f);
								gpu_mat.roughness_factor = 0.03f;
							}
							else {
								// Generic glass
								gpu_mat.base_color_factor = glm::vec4(0.92f, 0.96f, 1.0f, 0.08f);
								gpu_mat.roughness_factor = 0.04f;
							}
						}
						material_to_id[mi] = gpu_scene.register_material(gpu_mat);
					}

					for (size_t i = 0; i < mesh_data_copy->subsets.size(); ++i) {
						const auto& subset = mesh_data_copy->subsets[i];
						SubMesh sub;
						sub.index_start = subset.index_start;
						sub.index_count = subset.index_count;

						if (subset.material_index < material_to_id.size()) {
							sub.material_id = material_to_id[subset.material_index];
							sub.is_alpha_tested = mesh_data_copy->materials[subset.material_index].alpha_mode == 1; // 1 = AlphaMode::Mask
							sub.is_translucent = mesh_data_copy->materials[subset.material_index].alpha_mode == 2; // 2 = AlphaMode::Blend
						}
						else {
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
					sub.material_id = texture_slot_map.empty() ? 0 : texture_slot_map[0];
					new_mesh.submeshes.push_back(sub);
				}

				{
					std::lock_guard mesh_lock(mesh_mutex);
					if (assigned_mesh_id >= meshes.size())
						meshes.resize(assigned_mesh_id + 1);
					meshes[assigned_mesh_id] = std::move(new_mesh);
				}
			});
		}

		return { assigned_mesh_id, base_material_id };
	}

	void Renderer::flush_upload_queue() {
		auto queue = upload_queue;
		auto queue_ptr = queue;
		if (!queue_ptr) {
			std::string err = "Renderer::flush_upload_queue called but upload_queue is null";
			bud::eprint("{}", err);
#if defined(_DEBUG)
			throw std::runtime_error(err);
#else
			return;
#endif
		}

		std::vector<std::function<void()>> commands_to_run;
		{
			std::lock_guard lock(queue_ptr->mutex);
			if (queue_ptr->commands.empty()) {
				// No pending upload commands — not an error. Just return silently.
				return;
			}

			commands_to_run.swap(queue_ptr->commands);
		}

		for (const auto& rhi_cmd : commands_to_run) {
			if (rhi_cmd)
				rhi_cmd();
		}
	}

	void Renderer::enqueue_rhi_command(std::function<void()> cmd) {
		if (!upload_queue || !cmd)
			return;
		std::lock_guard lock(upload_queue->mutex);
		upload_queue->commands.push_back(std::move(cmd));
	}

	void Renderer::update_ui_draw_data(ImDrawData* draw_data) {
		if (ui_pass) {
			ui_pass->update_draw_data(draw_data);
		}
	}



	void Renderer::render(const bud::graphics::RenderScene& render_scene, SceneView& scene_view) {
		auto cmd = rhi->begin_frame();
		if (!cmd) {
			render_graph.reset();
			return;
		}

		// 处理当前帧挂起的上传任务（此时当前帧 in_flight_fence 已等待完成，命令池与 staging ring 已重置）
		flush_upload_queue();

		size_t instance_count = render_scene.instance_count.load(std::memory_order_relaxed);
		const uint32_t cascade_count = std::min(render_config.cascade_count, (uint32_t)MAX_CASCADES);
		uint32_t total_shadow_casters = 0;
		std::atomic<uint32_t> total_shadow_caster_submeshes{0};
		size_t visible_count = 0;
		size_t visible_instance_count = 0;
		size_t total_draw_count = 0;
		SceneDrawRanges ranges{};
		std::vector<std::vector<uint32_t>> culled_results(1 + cascade_count);

		// Advance 24H time cycle if enabled
		if (render_config.sky_config.enable_sky && render_config.sky_config.time_mode == SkyTimeMode::Cycle24H) {
			render_config.sky_config.time_of_day += scene_view.delta_time * render_config.sky_config.time_speed;
			if (render_config.sky_config.time_of_day >= 24.0f)
				render_config.sky_config.time_of_day = std::fmod(render_config.sky_config.time_of_day, 24.0f);
			else if (render_config.sky_config.time_of_day < 0.0f)
				render_config.sky_config.time_of_day += 24.0f;
		}

		uint32_t non_vg_total_count = 0;
		uint32_t non_vg_visible_count = 0;
		uint32_t vg_instance_count = 0;

		if (instance_count > 0) {
			update_cascades(scene_view, render_config, render_scene.scene_bounds);

			std::vector<bud::math::Frustum> view_frustums(1 + cascade_count);
			view_frustums[0].update(scene_view.view_proj_matrix, render_config.reversed_z);

			auto& main_visible_instances = culled_results[0];
			main_visible_instances.clear();

			for (size_t i = 0; i < render_scene.size(); ++i) {
				uint32_t mesh_id = render_scene.mesh_indices[i];
				if (mesh_id >= meshes.size())
					continue;
				const auto& mesh = meshes[mesh_id];

				if (mesh.is_page_based && render_config.enable_virtual_geometry) {
					// Plan A (UE5 Nanite aligned): Virtual Geometry is 100% GPU-driven.
					// All VG instances bypass CPU frustum culling and are dispatched to the GPU,
					// where hierarchy_traversal.comp and visibility.task perform GPU-driven frustum & occlusion culling.
					main_visible_instances.push_back(static_cast<uint32_t>(i));
					vg_instance_count++;
				} else {
					// Non-VG instances: CPU + GPU hybrid culling.
					// CPU performs coarse frustum culling against main camera frustum.
					non_vg_total_count++;
					if (bud::math::intersect_aabb_frustum(render_scene.world_aabbs[i], view_frustums[0])) {
						main_visible_instances.push_back(static_cast<uint32_t>(i));
						non_vg_visible_count++;
					}
				}
			}

			if (cascade_count == 0) {
				total_shadow_casters = static_cast<uint32_t>(main_visible_instances.size());
			}

			if (!main_visible_instances.empty() && cascade_count > 0) {
				for (uint32_t i = 0; i < cascade_count; ++i) {
					view_frustums[i + 1].update(scene_view.cascade_view_proj_matrices[i], render_config.reversed_z);
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
							uint32_t local_submeshes = 0;
							for (uint32_t instance : visible_instances) {
								uint32_t mesh_id = render_scene.mesh_indices[instance];
								if (mesh_id < meshes.size()) {
									const auto& mesh = meshes[mesh_id];
									local_submeshes += static_cast<uint32_t>(mesh.submeshes.size());
								}
							}
							total_shadow_caster_submeshes.fetch_add(local_submeshes, std::memory_order_relaxed);
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
			visible_instance_count = visible_instances.size();
			total_draw_count = 0;
			std::vector<uint32_t> draw_offsets(visible_instance_count + 1);
			for (size_t k = 0; k < visible_instance_count; ++k) {
				draw_offsets[k] = (uint32_t)total_draw_count;

				uint32_t i = visible_instances[k];
				uint32_t mesh_id = render_scene.mesh_indices[i];
				if (mesh_id >= meshes.size()) continue;
				const auto& mesh = meshes[mesh_id];
				uint32_t sub_idx = render_scene.submesh_indices[i];

				if (mesh.is_page_based) {
					// GPU-driven page-based mesh uses 1 sort item for hierarchy traversal
					total_draw_count += 1;
				}
				else if (sub_idx == bud::asset::INVALID_INDEX) {
					uint32_t sub_count = (uint32_t)mesh.submeshes.size();
					total_draw_count += (sub_count > 0) ? sub_count : 1u;
				}
				else {
					total_draw_count += 1;
				}
			}
			draw_offsets[visible_instance_count] = (uint32_t)total_draw_count;

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

			task_scheduler->ParallelFor(visible_instance_count, KEY_GEN_CHUNK_SIZE,
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
						if (mesh_id >= meshes.size())
							continue;
						const auto& mesh = meshes[mesh_id];
						uint32_t sub_idx_original = render_scene.submesh_indices[i];

						auto mesh_pos = bud::math::vec3(world_matrix[3]);
						auto distance = bud::math::distance2(mesh_pos, scene_view.camera_position);

						uint32_t depth_key = 0;
						auto depth_normalized = std::clamp(distance / (scene_view.far_plane * scene_view.far_plane), 0.0f, 1.0f);
						depth_key = static_cast<uint32_t>(depth_normalized * 0x3FFFF);

						if (mesh.is_page_based) {
							// GPU-driven page-based mesh: generate a single sort item
							// so the instance is included in the HierarchyInstance buffer
							// for hierarchy traversal. The actual draws come from the
							// GPU-written indirect draw buffer.
							auto& item = sort_list[draw_start];
							item.entity_index = (uint32_t)i;
							item.submesh_index = bud::asset::INVALID_INDEX;
							uint8_t layer = DRAW_LAYER_VIRTUAL_GEOMETRY;
							item.key = DrawKey::generate_opaque(layer, 0, 0, mesh_id, depth_key);
						}
						else if (sub_idx_original != bud::asset::INVALID_INDEX) {
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
								if (sub.is_translucent) {
									item.key = DrawKey::generate_translucent(DRAW_LAYER_TRANSLUCENT, std::sqrt(distance));
								}
								else {
									uint8_t layer = DRAW_LAYER_TRADITIONAL_OPAQUE;
									item.key = DrawKey::generate_opaque(layer, 0, sub.material_id, mesh_id, depth_key);
								}
							}
							else {
								uint32_t material_id = render_scene.material_indices[i];
								uint8_t layer = DRAW_LAYER_TRADITIONAL_OPAQUE;
								item.key = DrawKey::generate_opaque(layer, 0, material_id, mesh_id, depth_key);
							}
						}
						else {
							// Traditional mesh with submeshes: explode!
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
								if (sub.is_translucent) {
									item.key = DrawKey::generate_translucent(DRAW_LAYER_TRANSLUCENT, std::sqrt(distance));
								}
								else {
									uint8_t layer = DRAW_LAYER_TRADITIONAL_OPAQUE;
									item.key = DrawKey::generate_opaque(layer, 0, sub.material_id, mesh_id, depth_key);
								}
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

			auto end_it = std::remove_if(sort_list.begin(), sort_list.begin() + total_draw_count, [](const SortItem& a) { return a.key == UINT64_MAX; });
			sort_list.erase(end_it, sort_list.end()); // REMOVES INVALID ITEMS!
			visible_count = sort_list.size();

			for (size_t i = 0; i < visible_count; ++i) {
				uint8_t layer = static_cast<uint8_t>((sort_list[i].key >> 60) & 0xF);
				if (layer == DRAW_LAYER_VIRTUAL_GEOMETRY) {
					++ranges.range_a_count;
				} else if (layer == DRAW_LAYER_TRADITIONAL_OPAQUE) {
					++ranges.range_b_count;
				} else if (layer == DRAW_LAYER_TRANSLUCENT) {
					if (ranges.range_c_count == 0) {
						ranges.range_c_start = i;
					}
					++ranges.range_c_count;
				}
			}
			if (ranges.range_c_count == 0) {
				ranges.range_c_start = visible_count;
			}
		}

		rhi->set_render_config(render_config);

		auto swapchain_tex = rhi->get_current_swapchain_texture();

		if (rhi->is_headless()) {
			uint32_t width = rhi->get_width();
			uint32_t height = rhi->get_height();
			bool needs_recreate = !offscreen_target.is_valid();
			if (offscreen_target.is_valid()) {
				auto desc = rhi->get_texture_desc(offscreen_target);
				needs_recreate = desc.width != width || desc.height != height;
			}
			if (needs_recreate) {
				if (offscreen_target.is_valid()) {
					rhi->destroy_texture(offscreen_target);
					offscreen_target.reset();
				}

				if (width > 0 && height > 0) {
					bud::graphics::TextureDesc desc{};
					desc.width = width;
					desc.height = height;
					desc.format = bud::graphics::TextureFormat::RGBA8_SRGB;
					desc.is_transfer_src = true;
					desc.is_storage = true; // Just in case it's bound as storage
					offscreen_target = rhi->create_texture(desc, nullptr, 0);
					rhi->set_debug_name(offscreen_target, ObjectType::Texture, "HeadlessOffscreenTarget");
				}
			}
			swapchain_tex = offscreen_target;
		}

		if (!swapchain_tex.is_valid()) {
			render_graph.reset();
			return;
		}

		auto back_buffer = render_graph.import_texture("Backbuffer", swapchain_tex, ResourceState::Undefined);

		// Use the render-frame slot (NOT the swapchain image index) to index
		// per-frame GPU buffers: sync objects (in_flight_fence, upload timeline)
		// are per-slot, so image_index indexing would reuse buffers out of sync
		// with those fences (cross-frame race -> flicker).
		uint32_t current_idx = rhi->get_current_frame_index();
		RGHandle rg_draw;
		RGHandle rg_inst;
		RGHandle rg_stats;
		RGHandle rg_instance_data;
		// Full-scene shadow-caster DrawData (GPU-driven CSM cull input).
		RGHandle csm_instance_h;
		size_t scene_split = 0;
		size_t total_csm_items = 0;
		// Number of valid entities published into frame.csm_hierarchy_instances this
		// frame (full-scene shadow casters for the CSM cascade traversals).
		uint32_t csm_hierarchy_count = 0;
		for (size_t i = 0; i < instance_count; ++i) {
			uint32_t mid = render_scene.mesh_indices[i];
			if (mid < meshes.size() && meshes[mid].is_valid()) {
				if (meshes[mid].is_page_based) {
					total_csm_items += meshes[mid].submeshes.size();
				} else {
					++total_csm_items;
					++scene_split;
				}
			}
		}

		struct DrawData {
			uint32_t indexCount;
			uint32_t firstIndex;
			int32_t vertexOffset;
			uint32_t materialId;
			bud::math::vec3 min;
			uint32_t meshId;
			bud::math::vec3 max;
			uint32_t clusterStart;
			uint32_t clusterCount;
			uint32_t flags;
			uint32_t pageIndex;
			uint32_t visibilityOffset;
		};

		// Shadow-caster filter: is this (mesh[, submesh]) rendered as alpha-blended
		// translucency? Those objects never write shadow depth - they are drawn by the
		// forward translucent pass, and the shadow passes have no opacity-aware depth
		// write, so a glass pane would otherwise drop a fully opaque shadow.
		auto entity_is_translucent = [this](uint32_t mesh_index, uint32_t submesh_index = UINT32_MAX) -> bool {
			if (mesh_index >= meshes.size()) return false;
			const auto& m = meshes[mesh_index];
			if (!m.is_valid() || m.submeshes.empty()) return false;
			if (submesh_index != UINT32_MAX && submesh_index < m.submeshes.size())
				return m.submeshes[submesh_index].is_translucent;
			for (const auto& sub : m.submeshes) {
				if (sub.is_translucent) return true;
			}
			return false;
		};

		if (instance_count > 0) {
			gpu_scene.ensure_frame_resources(
				rhi,
				current_idx,
				static_cast<uint32_t>(visible_count),
				static_cast<uint32_t>(total_draw_count),
				static_cast<uint32_t>(std::max(instance_count, total_csm_items)),
				sizeof(InstanceData),
				sizeof(HierarchyInstance),
				sizeof(DrawData),
				sizeof(IndirectCommand),
				1024u);

			auto& frame = gpu_scene.get_frame_resources(current_idx);

			if (auto* req_buf = rhi->get_buffer(frame.page_request_readback); req_buf && req_buf->mapped_ptr && streaming_manager) {
				const uint32_t* buf = static_cast<const uint32_t*>(req_buf->mapped_ptr);
				uint32_t count = std::min(buf[0], 4093u);
				// buf[1] = overflow_count
				if (buf[1] > 0) {
					bud::print("[Renderer] Page request overflow: {} requests lost (buffer full)", buf[1]);
				}
				// buf[2] = error_flags (bit 0: stack overflow, bit 1: iteration limit)
				if (buf[2] & 1u) {
					bud::print("[Renderer] Hierarchy traversal stack overflow detected!");
				}
				if (buf[2] & 2u) {
					bud::print("[Renderer] Hierarchy traversal iteration limit exceeded!");
				}
				if (count > 0) {
					// requests start at buf[3] after request_count, overflow_count, error_flags
					streaming_manager->process_gpu_page_requests(buf + 3, count);
				}
			}

			auto& render_stats = rhi->get_render_stats();

			// Common Instance Data Upload
			// All per-frame staging->GPU copies go through the async upload
			// Per-frame data is host-written into mapped buffers (avoids async
			// staging/upload cross-queue timing); no upload command buffer used.
			if (visible_count > 0) {
				auto instance_staging = rhi->get_allocator()->alloc_staging(visible_count * sizeof(HierarchyInstance));
				HierarchyInstance* inst_mapped = static_cast<HierarchyInstance*>(instance_staging.mapped_ptr);
				for (size_t i = 0; i < visible_count; ++i) {
					const auto& item = sort_list[i];
					uint32_t entity_idx = item.entity_index;
					const auto& mesh = meshes[render_scene.mesh_indices[entity_idx]];

					inst_mapped[i].mesh_id = render_scene.mesh_indices[entity_idx];
					if (mesh.is_page_based && !mesh.submeshes.empty()) {
						// VG (page-based) meshes always have submesh_index = INVALID_INDEX in the
						// sort list. The correct base material ID is stored in submeshes[0].material_id
						// by the streaming manager during asset registration.
						inst_mapped[i].material_id = mesh.submeshes[0].material_id;
					}
					else if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
						inst_mapped[i].material_id = mesh.submeshes[item.submesh_index].material_id;
					}
					else {
						inst_mapped[i].material_id = render_scene.material_indices[entity_idx];
					}
					
					inst_mapped[i].root_group_index = render_scene.root_group_indices[entity_idx];
					inst_mapped[i].flags = render_scene.flags[entity_idx];
					
					const auto& aabb = render_scene.world_aabbs[entity_idx];
					inst_mapped[i].global_sphere_center = (aabb.min + aabb.max) * 0.5f;
					inst_mapped[i].global_sphere_radius = bud::math::length(aabb.max - aabb.min) * 0.5f;
					
					// Screen-space error threshold in pixels.
					// Matches the CPU-side select_page_lod() in types.hpp and the GPU-side
					// calculate_lod_error() in hierarchy_traversal.comp.
					inst_mapped[i].error_threshold = render_config.lod_error_threshold_px;
					inst_mapped[i].base_virtual_page = render_scene.base_virtual_pages[entity_idx];
					inst_mapped[i].model_matrix = render_scene.world_matrices[entity_idx];
				}
				// Write per-frame instance data directly into the host-visible
				// mapped buffer (avoids async staging/upload cross-queue timing).
				// The buffer is per-frame, so the CPU write only happens after
				// the previous frame (same slot) finished on the GPU (begin_frame
				// fence wait). Staging is only a CPU-side scratch buffer here.
				if (auto* buf = rhi->get_buffer(frame.instance_data); buf && buf->mapped_ptr) {
					std::memcpy(buf->mapped_ptr, inst_mapped, visible_count * sizeof(HierarchyInstance));
				}
				rg_instance_data = render_graph.import_buffer("GlobalInstanceData", frame.instance_data, ResourceState::ShaderResource);
			}

			const BufferHandle current_inst_buf = frame.indirect_instance;
			const BufferHandle current_draw_buf = frame.indirect_draw;
			const BufferHandle current_stats_buf = frame.stats_readback;

				if (visible_count > 0) {
					auto staging = rhi->get_allocator()->alloc_staging(visible_count * sizeof(DrawData));
					DrawData* mapped = static_cast<DrawData*>(staging.mapped_ptr);

					for (size_t i = 0; i < visible_count; ++i) {
						uint32_t entity_idx = sort_list[i].entity_index;
						uint32_t mesh_index = render_scene.mesh_indices[entity_idx];
						const auto& mesh = meshes[mesh_index];
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_index);

						mapped[i].meshId = mesh_index;
						mapped[i].pageIndex = mesh.page_index;
						mapped[i].clusterStart = 0;
						mapped[i].clusterCount = 0;
						mapped[i].visibilityOffset = 0;

						uint32_t sub_idx = sort_list[i].submesh_index;
						uint32_t index_start = 0;
						uint32_t index_count = mesh.index_count;
						uint32_t mat_id = render_scene.material_indices[entity_idx];
						bud::math::AABB local_aabb = mesh.aabb;

						if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
							const auto& sub = mesh.submeshes[sub_idx];
							index_count = sub.index_count;
							index_start = sub.index_start;
							mat_id = sub.material_id;
							local_aabb = sub.aabb;
						}

						mapped[i].indexCount = index_count;
						mapped[i].firstIndex = mesh_geometry.first_index + index_start;
						mapped[i].vertexOffset = mesh.is_page_based ? 0 : mesh_geometry.vertex_offset;
						mapped[i].materialId = mat_id;

						auto world_aabb = local_aabb.transform(render_scene.world_matrices[entity_idx]);
						mapped[i].min = world_aabb.min;
						mapped[i].max = world_aabb.max;
						// DrawData.flags: bit0 page-based, bit1 static, bit2 = no-cast-shadow
						// (mirrors RenderScene::INSTANCE_FLAG_NO_CAST_SHADOW == 2).
						mapped[i].flags = (mesh.is_page_based ? 1u : 0u) | ((render_scene.flags[entity_idx] & 1) ? 2u : 0u) | ((render_scene.flags[entity_idx] & 2) ? 4u : 0u);
					}

					if (auto* buf = rhi->get_buffer(current_inst_buf); buf && buf->mapped_ptr) {
						std::memcpy(buf->mapped_ptr, mapped, visible_count * sizeof(DrawData));
					}

					rg_inst = render_graph.import_buffer("IndirectInstanceData", current_inst_buf, ResourceState::UnorderedAccess);
					rg_draw = render_graph.import_buffer("IndirectDrawCommands", current_draw_buf, ResourceState::IndirectArgument);
					rg_stats = render_graph.import_buffer("GPUStatsReadback", current_stats_buf, ResourceState::UnorderedAccess);
				}

				if (total_csm_items > 0 && frame.csm_instance_data.is_valid()) {
					const size_t scene_count = total_csm_items;
					auto* buf_scene = rhi->get_buffer(frame.csm_instance_data);
					DrawData* scene_mapped = (buf_scene && buf_scene->mapped_ptr) ? static_cast<DrawData*>(buf_scene->mapped_ptr) : nullptr;

					auto* buf_models = rhi->get_buffer(frame.csm_instance_models);
					InstanceData* instance_dst = (buf_models && buf_models->mapped_ptr) ? static_cast<InstanceData*>(buf_models->mapped_ptr) : nullptr;

					if (scene_mapped)
						std::memset(scene_mapped, 0, scene_count * sizeof(DrawData));
					if (instance_dst)
						std::memset(instance_dst, 0, scene_count * sizeof(InstanceData));

					auto* pt_buf = rhi->get_buffer(gpu_scene.get_page_table_buffer());
					const auto* pt_entries = (pt_buf && pt_buf->mapped_ptr) ? static_cast<const GPUScene::PageTableEntry*>(pt_buf->mapped_ptr) : nullptr;

					uint32_t static_idx = 0;
					uint32_t page_idx = static_cast<uint32_t>(scene_split);

					// Two passes make the TRADITIONAL caster block prefix-contiguous:
					// pass 0 writes everything allowed to cast (plus all page-based entries,
					// whose index range is independent), pass 1 writes the traditional
					// translucent entries that must not cast. static_idx / page_idx are
					// declared OUTSIDE the pass loop so the page region stays untouched and
					// the traditional block ends up [0, castable) followed by the skipped ones.
					// CSMShadowPass can then issue a plain [first, first + count) range
					// instead of filtering every indirect command.
					const uint32_t caster_passes = render_config.shadow_translucent_casters ? 1u : 2u;
					for (uint32_t caster_pass = 0; caster_pass < caster_passes; ++caster_pass)
					for (size_t i = 0; i < instance_count; ++i) {
						uint32_t entity_idx = static_cast<uint32_t>(i);
						uint32_t mesh_index = render_scene.mesh_indices[entity_idx];
						
						if (mesh_index >= meshes.size() || !meshes[mesh_index].is_valid()) continue;
						
						const auto& mesh = meshes[mesh_index];
						const auto& mesh_geometry = gpu_scene.get_mesh_geometry(mesh_index);

						// Pass filter: pass 0 keeps castable entries (and every page-based one,
						// which the two passes must not reorder), pass 1 keeps only the
						// traditional translucent entries that must not cast.
						if (caster_passes > 1) {
							const bool traditional = !mesh.is_page_based;
							const bool castable = !traditional ||
								!entity_is_translucent(mesh_index, render_scene.submesh_indices[entity_idx]);
							const bool wanted = (caster_pass == 0) ? castable : (traditional && !castable);
							if (!wanted) continue;
						}

						if (mesh.is_page_based) {
							for (size_t s = 0; s < mesh.submeshes.size(); ++s) {
								const auto& sub = mesh.submeshes[s];
								uint32_t write_idx = page_idx++;
								uint32_t vpage = sub.page_index;
								uint32_t physical_slot = ~0u;
								bool is_resident = false;

								if (pt_entries && vpage < GPUScene::max_page_table_entries) {
									is_resident = (pt_entries[vpage].valid != 0);
									physical_slot = pt_entries[vpage].pool_offset;
								}

								DrawData d{};
								d.meshId = mesh_index;
								d.flags = 1u | ((render_scene.flags[entity_idx] & 1) ? 2u : 0u) | ((render_scene.flags[entity_idx] & 2) ? 4u : 0u);
								d.clusterStart = 0;
								d.clusterCount = 0;
								d.visibilityOffset = 0;
								d.vertexOffset = 0;
								d.materialId = sub.material_id;

								if (is_resident && physical_slot != ~0u) {
									d.indexCount = sub.index_count;
									d.firstIndex = (physical_slot * 131072u) / 2u + sub.index_start;
									d.pageIndex = physical_slot;
								} else {
									d.indexCount = 0;
									d.firstIndex = 0;
									d.pageIndex = ~0u;
								}

								auto world_sub_aabb = sub.aabb.transform(render_scene.world_matrices[entity_idx]);
								d.min = world_sub_aabb.min;
								d.max = world_sub_aabb.max;

								if (scene_mapped)
									scene_mapped[write_idx] = d;

								if (instance_dst) {
									instance_dst[write_idx].model = render_scene.world_matrices[entity_idx];
									instance_dst[write_idx].material_id = sub.material_id;
									instance_dst[write_idx].page_slot = (is_resident && physical_slot != ~0u) ? physical_slot : ~0u;
									instance_dst[write_idx].blend_factor = 0.0f;
									instance_dst[write_idx].padding = 0;
								}
							}
						} else {
							uint32_t write_idx = static_idx++;
							DrawData d{};
							d.meshId = mesh_index;
							d.flags = ((render_scene.flags[entity_idx] & 1) ? 2u : 0u) | ((render_scene.flags[entity_idx] & 2) ? 4u : 0u);
							d.pageIndex = ~0u;
							d.clusterStart = 0;
							d.clusterCount = 0;
							d.visibilityOffset = 0;

							uint32_t sub_idx = render_scene.submesh_indices[entity_idx];
							uint32_t index_start = 0;
							uint32_t index_count = mesh.index_count;
							uint32_t mat_id = render_scene.material_indices[entity_idx];
							bud::math::AABB local_aabb = mesh.aabb;

							if (sub_idx != bud::asset::INVALID_INDEX && sub_idx < mesh.submeshes.size()) {
								const auto& sub = mesh.submeshes[sub_idx];
								index_count = sub.index_count;
								index_start = sub.index_start;
								mat_id = sub.material_id;
								local_aabb = sub.aabb;
							}
							d.indexCount = index_count;
							d.firstIndex = mesh_geometry.first_index + index_start;
							d.vertexOffset = mesh_geometry.vertex_offset;
							d.materialId = mat_id;

							auto world_aabb = local_aabb.transform(render_scene.world_matrices[entity_idx]);
							d.min = world_aabb.min;
							d.max = world_aabb.max;

							if (scene_mapped)
								scene_mapped[write_idx] = d;

							if (instance_dst) {
								instance_dst[write_idx].model = render_scene.world_matrices[entity_idx];
								instance_dst[write_idx].material_id = mat_id;
								instance_dst[write_idx].page_slot = ~0u;
								instance_dst[write_idx].blend_factor = 0.0f;
								instance_dst[write_idx].padding = 0;
							}
						}
					}

					csm_instance_h = render_graph.import_buffer("CSMSceneInstanceData", frame.csm_instance_data, ResourceState::UnorderedAccess);
				}

				// --- Full-scene HierarchyInstance list for shadow casters -----------------
				// The CSM cascade traversals used to walk frame.instance_data, which only
				// holds the instances visible to the MAIN camera. Rotating a caster out of
				// view therefore removed it from the shadow map and its shadow vanished
				// with it. A shadow map needs every caster inside the cascade box, whether
				// or not the player can see it; hierarchy_traversal.comp already
				// frustum-culls each instance against the cascade it is given, so walking
				// the whole scene only costs one sphere test per off-screen entity.
				csm_hierarchy_count = 0;
				if (render_config.shadow_full_scene_casters &&
					render_config.enable_virtual_geometry && cascade_count > 0 &&
					frame.csm_hierarchy_instances.is_valid()) {
					if (auto* hbuf = rhi->get_buffer(frame.csm_hierarchy_instances); hbuf && hbuf->mapped_ptr) {
						auto* hi_mapped = static_cast<HierarchyInstance*>(hbuf->mapped_ptr);
						// instance_id is packed into 16 bits of the visible-page entry.
						const size_t hi_limit = std::min<uint64_t>(frame.csm_hierarchy_capacity, 65536ull);
						const size_t scene_n = std::min({
							instance_count,
							render_scene.world_matrices.size(),
							render_scene.world_aabbs.size(),
							render_scene.mesh_indices.size(),
							render_scene.material_indices.size(),
							render_scene.flags.size(),
							render_scene.root_group_indices.size(),
							render_scene.base_virtual_pages.size(),
							hi_limit
							});
						csm_hierarchy_count = static_cast<uint32_t>(scene_n);
						for (size_t i = 0; i < scene_n; ++i) {
							HierarchyInstance hi{};
							hi.model_matrix = bud::math::mat4(1.0f);
							hi.global_sphere_center = bud::math::vec3(0.0f);
							hi.global_sphere_radius = 0.0f;
							hi.root_group_index = bud::asset::INVALID_INDEX;

							const uint32_t mesh_index = render_scene.mesh_indices[i];
							if (mesh_index < meshes.size() && meshes[mesh_index].is_valid()) {
								const auto& mesh = meshes[mesh_index];
								hi.model_matrix = render_scene.world_matrices[i];
								hi.mesh_id = mesh_index;
								hi.flags = render_scene.flags[i];
								hi.error_threshold = render_config.lod_error_threshold_px;
								if (mesh.is_page_based && !mesh.submeshes.empty()) {
									// VG (page-based) mesh: base material lives in submeshes[0],
									// same convention as the main-view fill above.
									hi.material_id = mesh.submeshes[0].material_id;
									hi.root_group_index = render_scene.root_group_indices[i];
									hi.base_virtual_page = render_scene.base_virtual_pages[i];
								}
								else {
									// Traditional mesh: no VG hierarchy -> the traversal
									// early-outs, the shadow is drawn by the vertex path.
									hi.material_id = render_scene.material_indices[i];
								}
								const auto& aabb = render_scene.world_aabbs[i];
								hi.global_sphere_center = (aabb.min + aabb.max) * 0.5f;
								hi.global_sphere_radius = bud::math::length(aabb.max - aabb.min) * 0.5f;
							}
							hi_mapped[i] = hi;
						}
					}
				}

				// --- Throttled shadow-caster diagnostic -------------------------------
				// Deliberately independent of shadow_full_scene_casters: it reports which
				// objects feed the cascades and which opted out, so an accidental backdrop
				// lid (e.g. y=[13.30,14.20] x=[-19.2,18.0] z=[-11.8,11.1]) can be spotted
				// and marked "is_cast_shadow": false in the scene file.
				static float s_next_caster_print = -1.0f;
				if (scene_view.time >= s_next_caster_print) {
					s_next_caster_print = scene_view.time + 1.0f;
					const size_t flag_n = std::min({
						instance_count,
						render_scene.flags.size(),
						render_scene.world_aabbs.size(),
						render_scene.mesh_indices.size()
						});
					size_t no_cast = 0;
					for (size_t i = 0; i < flag_n; ++i) {
						if (render_scene.flags[i] & RenderScene::INSTANCE_FLAG_NO_CAST_SHADOW)
							++no_cast;
					}
					// bud::print("[CSM] shadow casters: {} (scene instances={}, main-view visible={}, opted out of casting={})",
					// 	render_config.shadow_full_scene_casters ? "FULL SCENE" : "main-view only",
					// 	flag_n, visible_count, no_cast);

					// Backdrop discovery deliberately lives in BudEngine::extract_scene,
					// where the entity's asset_path is available: the instance index here
					// is produced by a ParallelFor over an atomic counter, so it is not
					// stable and cannot be used to point back into the scene file.
				}

				// Read back previous frame stats (delayed latency) from this exact buffer which is guaranteed finished
				if (frame.stats_readback.is_valid()) {
					if (auto* buf = rhi->get_buffer(frame.stats_readback); buf && buf->mapped_ptr) {
						GPUStats* gpu_stats = static_cast<GPUStats*>(buf->mapped_ptr);
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
				}
				else {
					scene_total_objs += 1;
					if (render_scene.submesh_indices[i] != bud::asset::INVALID_INDEX && render_scene.submesh_indices[i] < meshes[mesh_id].submeshes.size()) {
						scene_total_tris += meshes[mesh_id].submeshes[render_scene.submesh_indices[i]].index_count / 3;
					}
					else {
						for (const auto& sub : meshes[mesh_id].submeshes) {
							scene_total_tris += sub.index_count / 3;
						}
					}
				}
			}

			uint32_t cpu_total_instances = static_cast<uint32_t>(total_draw_count);
			uint32_t cpu_visible_instances = static_cast<uint32_t>(visible_count);
			uint32_t cpu_total_clusters = 0;
			uint32_t cpu_visible_clusters = 0;

			uint32_t cpu_total_tris = 0;
			uint32_t cpu_visible_tris = 0;
			for (size_t i = 0; i < visible_count; ++i) {
				const auto& item = sort_list[i];
				if (item.entity_index == UINT32_MAX && item.key == UINT64_MAX) continue;
				uint32_t mesh_id = render_scene.mesh_indices[item.entity_index];
				if (mesh_id >= meshes.size()) continue;

				uint32_t tris = 0;

				if (item.submesh_index != UINT32_MAX && item.submesh_index < meshes[mesh_id].submeshes.size()) {
					const auto& sub = meshes[mesh_id].submeshes[item.submesh_index];
					tris = sub.index_count / 3;
				}
				else {
					tris = meshes[mesh_id].index_count / 3;
				}

				cpu_total_tris += tris;
				if (i < visible_count) {
					cpu_visible_tris += tris;
				}
			}

			bool has_main_pass = false;
			RGHandle shadow_map;
			bool is_mesh_shader_vg = (has_mesh_shader && render_config.enable_mesh_shader && visibility_pass && resolve_pass && render_config.enable_virtual_geometry);

			// Setup GPU Stats
			rhi->add_culling_stats(vg_instance_count > 0 ? vg_instance_count : scene_total_objs, (uint32_t)visible_instance_count, total_shadow_casters);
			if (is_mesh_shader_vg) {
				uint32_t vg_visible_pages = 0;
				uint32_t vg_visible_tris = 0;
				if (frame.visible_pages_readback.is_valid()) {
					if (auto* vp_buf = rhi->get_buffer(frame.visible_pages_readback); vp_buf && vp_buf->mapped_ptr) {
						const uint32_t* vp_data = static_cast<const uint32_t*>(vp_buf->mapped_ptr);
						vg_visible_pages = std::min(vp_data[0], bud::asset::VG_MAX_VISIBLE_PAGES);

						if (auto* pool_buf = rhi->get_buffer(gpu_scene.get_page_pool_buffer()); pool_buf && pool_buf->mapped_ptr) {
							const uint8_t* pool_base = static_cast<const uint8_t*>(pool_buf->mapped_ptr);
							for (uint32_t p = 0; p < vg_visible_pages; ++p) {
								uint32_t pack = vp_data[1 + p];
								uint32_t page_slot = pack & bud::asset::VG_PAGE_SLOT_MASK;
								if (page_slot < GPUScene::PagePool::max_pages) {
									const auto* ph = reinterpret_cast<const bud::asset::VGPageDataHeader*>(pool_base + page_slot * GPUScene::PagePool::page_size);
									if (ph && ph->magic == bud::asset::VG_PAGE_MAGIC) {
										vg_visible_tris += ph->index_count / 3;
									}
								}
							}
						}
						if (vg_visible_tris == 0 && vg_visible_pages > 0) {
							vg_visible_tris = vg_visible_pages * bud::asset::VG_DEFAULT_ESTIMATED_PAGE_TRIANGLES;
						}
					}
				}
				rhi->get_render_stats().gpu_total_instances = static_cast<uint32_t>(render_scene.size());
				rhi->get_render_stats().gpu_visible_instances = vg_visible_pages;
				rhi->get_render_stats().gpu_total_triangles = scene_total_tris;
				rhi->get_render_stats().gpu_visible_triangles = vg_visible_tris;
				rhi->get_render_stats().heuristic_total_count = 0;
				rhi->get_render_stats().heuristic_cutoff_bucket = 0;
				rhi->get_render_stats().heuristic_remaining = 0;
				rhi->get_render_stats().gpu_occluder_instances = 0;
				rhi->get_render_stats().active_visibility_path = bud::graphics::VisibilityPath::Cluster;
			}
			else if (visible_count > 0) {
				rhi->get_render_stats().gpu_total_instances = last_gpu_stats.totalInstances;
				rhi->get_render_stats().gpu_visible_instances = last_gpu_stats.visibleInstances;
				rhi->get_render_stats().gpu_total_triangles = last_gpu_stats.totalTriangles;
				rhi->get_render_stats().gpu_visible_triangles = last_gpu_stats.visibleTriangles;
				rhi->get_render_stats().heuristic_total_count = last_gpu_stats.heuristicTotalCount;
				rhi->get_render_stats().heuristic_cutoff_bucket = last_gpu_stats.heuristicCutoffBucket;
				rhi->get_render_stats().heuristic_remaining = last_gpu_stats.heuristicRemaining;
				rhi->get_render_stats().gpu_occluder_instances = last_gpu_stats.heuristicVisibleInstances;
				rhi->get_render_stats().active_visibility_path = render_config.enable_virtual_geometry ? bud::graphics::VisibilityPath::Cluster : bud::graphics::VisibilityPath::Instance;
			}
			else {
				last_gpu_stats = {};
				rhi->get_render_stats().gpu_total_instances = 0;
				rhi->get_render_stats().gpu_visible_instances = 0;
				rhi->get_render_stats().gpu_total_triangles = 0;
				rhi->get_render_stats().gpu_visible_triangles = 0;
				rhi->get_render_stats().heuristic_total_count = 0;
				rhi->get_render_stats().heuristic_cutoff_bucket = 0;
				rhi->get_render_stats().heuristic_remaining = 0;
				rhi->get_render_stats().gpu_occluder_instances = 0;
			}

			// Push CPU Stats (Non-VG)
			rhi->get_render_stats().cpu_total_objects = non_vg_total_count;
			rhi->get_render_stats().cpu_visible_objects = non_vg_visible_count;
			rhi->get_render_stats().cpu_total_instances = non_vg_total_count;
			rhi->get_render_stats().cpu_visible_instances = non_vg_visible_count;
			rhi->get_render_stats().cpu_total_triangles = scene_total_tris;
			rhi->get_render_stats().cpu_visible_triangles = cpu_visible_tris;

			// Push shadow caster stats to RenderStats
			rhi->get_render_stats().shadow_casters = total_shadow_casters;
			rhi->get_render_stats().shadow_caster_submeshes = total_shadow_caster_submeshes;

			if (rg_instance_data.is_valid()) {
				rhi->update_global_instance_data(is_mesh_shader_vg ? frame.instance_data : frame.dynamic_instances);
			}

			if (frame.csm_instance_models.is_valid()) {
				rhi->update_global_csm_instance_data(frame.csm_instance_models);
			}

			rhi->update_global_page_table(gpu_scene.get_page_table_buffer());
			rhi->update_global_page_pool(gpu_scene.get_page_pool_buffer());
			rhi->update_global_materials_buffer(gpu_scene.get_materials_buffer());
			if (visible_count > 0) {
				std::vector<std::vector<uint32_t>> csm_visible_instances(cascade_count);
				for (uint32_t i = 0; i < cascade_count; ++i)
					csm_visible_instances[i] = std::move(culled_results[i + 1]);

				const RGHandle csm_inst_input = csm_instance_h.is_valid() ? csm_instance_h : rg_inst;
				const size_t csm_inst_count = csm_instance_h.is_valid() ? total_csm_items : visible_count;
				const size_t csm_split = csm_instance_h.is_valid() ? scene_split : ranges.range_a_count;

				// --- Shadow caster lists -------------------------------------------------
				// Translucent meshes are drawn by the forward translucent pass and must not
				// rasterize into the shadow map: the shadow passes have no opacity-aware depth
				// stage, so a glass pane would drop a fully solid shadow.
				//
				// The traditional block of the full-scene list is written in TWO PASSES (see
				// the fill loop above) so everything allowed to cast occupies the PREFIX
				// [0, command_count) of each cascade block. That keeps the issued range
				// contiguous and avoids filtering per command. (The per-instance
				// "is_cast_shadow" opt-out is a different mechanism: it is evaluated by
				// csm_cull.comp, which emits instance_count = 0 for those slots.)
				//
				// csm_cull.comp lays out cascade blocks of exactly `total_instances` entries,
				// so the reader must use the very same stride; guessing it from a related but
				// different count is what corrupted every cascade >= 1 previously.
				CSMShadowPass::ShadowCasterLists csm_casters;
				const bool is_csm_vg = render_config.enable_virtual_geometry && has_mesh_shader;
				const size_t csm_cull_total = is_csm_vg ? csm_split : csm_inst_count;
				const bool skip_translucent_casters = !render_config.shadow_translucent_casters;
				csm_casters.skip_translucent_casters = skip_translucent_casters;
				csm_casters.traditional.stride_commands = static_cast<uint32_t>(csm_cull_total);

				if (is_csm_vg) {
					// Full-scene list layout after the two-pass fill:
					//   [0, castable)          traditional casters      <- issue these
					//   [castable, scene_split) traditional, no casting
					//   [scene_split, total)    page-based (rasterized by the mesh path)
					size_t trad_castable = 0;
					if (skip_translucent_casters) {
						for (size_t e = 0; e < instance_count; ++e) {
							if (e >= render_scene.mesh_indices.size()) continue;
							const uint32_t mi = render_scene.mesh_indices[e];
							if (mi >= meshes.size() || !meshes[mi].is_valid() || meshes[mi].is_page_based) continue;
							const uint32_t si = (e < render_scene.submesh_indices.size())
								? render_scene.submesh_indices[e] : UINT32_MAX;
							if (entity_is_translucent(mi, si)) continue;
							++trad_castable;
						}
					}
					else {
						trad_castable = scene_split;
					}
					csm_casters.traditional.command_count = static_cast<uint32_t>(trad_castable);
				}
				else {
					// Sorted visible list: [0, range_a) is the VG layer, [range_a, range_a +
					// range_b) the traditional opaque/masked range, then the translucent
					// layer last. Previously this branch issued range A (the VG items!)
					// through the traditional pipeline, or fell through to a legacy path that
					// bound the page pool as a vertex buffer.
					csm_casters.traditional.first_command = static_cast<uint32_t>(ranges.range_a_count);
					size_t trad_castable = 0;
					for (size_t k = ranges.range_a_count; k < visible_count && k < sort_list.size(); ++k) {
						const uint32_t ei = sort_list[k].entity_index;
						if (ei == UINT32_MAX) break;
						if (ei >= render_scene.mesh_indices.size()) continue;
						const uint32_t mi = render_scene.mesh_indices[ei];
						if (mi >= meshes.size() || !meshes[mi].is_valid()) continue;
						if (meshes[mi].is_page_based) continue;	// not part of range B
						if (skip_translucent_casters && entity_is_translucent(mi, sort_list[k].submesh_index))
							break;								// translucent range is contiguous
						++trad_castable;
					}
					csm_casters.traditional.command_count = static_cast<uint32_t>(trad_castable);
				}

				RGHandle rg_csm_indirect;
				// GPU-driven CSM culling for traditional dynamic meshes (only when dynamic instances exist)
				if (csm_cull_pipeline.is_valid() && frame.csm_indirect_draw.is_valid()) {
					if (!is_csm_vg || csm_cull_total > 0) {
						rg_csm_indirect = render_graph.import_buffer("CSMIndirectDraw", frame.csm_indirect_draw, ResourceState::UnorderedAccess);
						render_graph.add_pass("CSM Cull",
							[=](RGBuilder& builder) {
								builder.read(csm_inst_input, ResourceState::ShaderResource);
								builder.write(rg_csm_indirect, ResourceState::UnorderedAccess);
							},
							[=, this](RHI* rhi, CommandHandle cmd) {
								rhi->cmd_bind_pipeline(cmd, csm_cull_pipeline);
								rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 0, render_graph.get_buffer(csm_inst_input));
								rhi->cmd_bind_storage_buffer(cmd, csm_cull_pipeline, 2, frame.csm_indirect_draw);
								rhi->cmd_bind_compute_ubo(cmd, csm_cull_pipeline, 4);
								struct PushConsts {
									uint32_t total_instances;
									uint32_t did_copy;
									uint32_t static_only;
								} pc;
								pc.total_instances = static_cast<uint32_t>(csm_cull_total);
								pc.did_copy = 0;
								pc.static_only = 0;
								rhi->cmd_push_constants(cmd, csm_cull_pipeline, sizeof(PushConsts), &pc);
								rhi->cmd_dispatch(cmd, (pc.total_instances + 255) / 256, 1, 1);
							}
						);
					}
				}

				std::array<RGHandle, MAX_CASCADES> rg_csm_visible_pages{};

				// Which VG instance list the shadow cascades walk. This MUST be the same
				// buffer handed to CSMShadowPass below: the per-cascade visible-page list
				// stores instance_id indexes into it, so a mismatch would draw casters
				// with other objects' transforms.
				const bool full_scene_shadow_casters =
					render_config.shadow_full_scene_casters && csm_hierarchy_count > 0;
				const uint32_t vg_shadow_instance_count = full_scene_shadow_casters
					? csm_hierarchy_count
					: static_cast<uint32_t>(visible_count);
				const BufferHandle vg_shadow_instances = full_scene_shadow_casters
					? frame.csm_hierarchy_instances
					: BufferHandle{};

				if (render_config.enable_virtual_geometry && hierarchy_traversal_pass) {
					for (uint32_t c_idx = 0; c_idx < cascade_count; ++c_idx) {
						float lod_error_scale = 1.0f;

						if (c_idx == 1)
							lod_error_scale = 2.5f;
						else if (c_idx == 2)
							lod_error_scale = 5.0f;
						else if (c_idx >= 3)
							lod_error_scale = 10.0f;

						// The LOD metric must run on the REAL ortho extent update_cascades()
						// derived for this cascade (texel footprint * map width), not on the
						// static shadow_ortho_size * 2^i ladder which had no relation to the
						// actual boxes: casters ended up rasterized with far too coarse (or
						// far too fine) page LODs, and the mismatch grew with scene scale.
						float real_extent = scene_view.cascade_texel_size[c_idx] * static_cast<float>(render_config.shadow_map_size);
						float ortho_extent = (real_extent > 0.0f)
							? real_extent
							: render_config.shadow_ortho_size * std::pow(2.0f, static_cast<float>(c_idx));
						std::string pass_name = "CSM Cascade " + std::to_string(c_idx) + " Traversal";
						rg_csm_visible_pages[c_idx] = hierarchy_traversal_pass->add_to_graph(
							render_graph,
							scene_view,
							render_config,
							render_scene,
							meshes,
							vg_shadow_instance_count,
							gpu_scene,
							current_idx,
							c_idx + 1,
							lod_error_scale,
							ortho_extent,
							frame.csm_visible_pages[c_idx],
							pass_name,
							vg_shadow_instances
						);
					}
				}

				shadow_map = csm_pass->add_to_graph(render_graph, scene_view, render_config, render_scene, meshes, std::move(csm_visible_instances), gpu_scene, gpu_scene.get_vertex_buffer(), gpu_scene.get_index_buffer(), csm_inst_input, csm_casters, rg_csm_indirect, rg_csm_visible_pages, vg_shadow_instances);

				if (is_mesh_shader_vg) {
					// Mesh shader visibility path (task+mesh shader)
					RGHandle rg_visible_pages{};
					if (hierarchy_traversal_pass)
						rg_visible_pages = hierarchy_traversal_pass->add_to_graph(render_graph, scene_view, render_config, render_scene, meshes, visible_count, gpu_scene, current_idx);

					gpu_scene.ensure_hiz_textures(rhi, scene_view.viewport_width, scene_view.viewport_height);
					gpu_scene.ensure_color_textures(rhi, scene_view.viewport_width, scene_view.viewport_height);

					RGHandle rg_history_hiz{};
					if (gpu_scene.has_history_hiz() && render_config.enable_hiz_culling) {
						auto hist_tex = gpu_scene.get_history_hiz(current_idx);
						if (hist_tex.is_valid())
							rg_history_hiz = render_graph.import_texture("HistoryHiZ", hist_tex, ResourceState::ShaderResource);
					}

					RGHandle rg_history_color{};
					if (gpu_scene.has_history_color() && (render_config.enable_ssr || render_config.enable_ssgi)) {
						auto hist_col_tex = gpu_scene.get_history_color(current_idx);
						if (hist_col_tex.is_valid())
							rg_history_color = render_graph.import_texture("HistorySceneColor", hist_col_tex, ResourceState::ShaderResource);
					}

					if (rg_visible_pages.is_valid() && visibility_pass) {
						RGHandle rg_depth{};
						// Phase 1: Visibility Pass using History Hi-Z
						auto rg_visibility = visibility_pass->add_to_graph(render_graph, back_buffer, rg_depth,
							scene_view, render_config, rg_visible_pages, rg_history_hiz, gpu_scene, ranges,
							gpu_scene.get_vertex_buffer(), gpu_scene.get_index_buffer(), rg_draw, &rg_depth);

						// Build Current Frame Hi-Z Pyramid from Phase 1 Depth Buffer
						RGHandle rg_current_hiz{};
						if (rg_depth.is_valid() && pyramid_mip_pass) {
							RGHandle target_hiz{};
							auto curr_tex = gpu_scene.get_current_hiz(current_idx);
							if (curr_tex.is_valid())
								target_hiz = render_graph.import_texture("CurrentHiZ", curr_tex, ResourceState::Undefined);
							rg_current_hiz = pyramid_mip_pass->add_to_graph(render_graph, rg_depth, render_config, target_hiz);
						}

						// Phase 2: Incremental Visibility Pass using Current Hi-Z
						if (rg_current_hiz.is_valid()) {
							gpu_scene.mark_history_hiz_valid();
							visibility_pass->add_phase2_to_graph(render_graph, rg_visibility, rg_depth,
								scene_view, render_config, rg_visible_pages, rg_current_hiz, gpu_scene);

							if (render_config.debug_hiz && pyramid_mip_debug_pass)
								pyramid_mip_debug_pass->add_to_graph(render_graph, back_buffer, rg_current_hiz, render_config.debug_hiz_mip);
						}

						RGHandle rg_ao{};
						if (rg_depth.is_valid() && ao_pass && render_config.ao_mode != AOMode::Disabled) {
							RGHandle raw_ao = ao_pass->add_to_graph(render_graph, rg_depth, scene_view, render_config);
							if (raw_ao.is_valid() && ao_temporal_pass)
								raw_ao = ao_temporal_pass->add_to_graph(render_graph, raw_ao, rg_depth, scene_view, render_config);
							if (raw_ao.is_valid() && ao_blur_pass)
								rg_ao = ao_blur_pass->add_to_graph(render_graph, raw_ao, rg_depth, scene_view, render_config);
							else
								rg_ao = raw_ao;
						}

						RGHandle rg_ssr{};
						if (rg_depth.is_valid() && ssr_pass && render_config.enable_ssr) {
							rg_ssr = ssr_pass->add_to_graph(render_graph, rg_depth, rg_history_color, scene_view, render_config);
						}

						RGHandle rg_ssgi{};
						if (rg_depth.is_valid() && ssgi_pass && render_config.enable_ssgi) {
							rg_ssgi = ssgi_pass->add_to_graph(render_graph, rg_depth, rg_history_color, scene_view, render_config);
						}

						if (rg_visibility.is_valid() && resolve_pass) {
							resolve_pass->add_to_graph(render_graph, back_buffer, rg_visibility,
								scene_view, render_config, gpu_scene, shadow_map, rg_ao, rg_ssr, rg_ssgi);
							has_main_pass = true;

							if (physics_debug_pass && render_config.debug_physics) {
								physics_debug_pass->add_to_graph(render_graph, back_buffer, rg_depth,
									scene_view, render_config);
							}

							if (forward_translucent_pass && ranges.range_c_count > 0) {
								forward_translucent_pass->add_to_graph(render_graph, shadow_map, back_buffer, rg_depth,
									render_scene, scene_view, render_config, meshes, sort_list,
									ranges, rg_draw, rg_inst, gpu_scene,
									gpu_scene.get_vertex_buffer(), gpu_scene.get_index_buffer(),
									rg_ao, rg_ssr, rg_ssgi, rg_history_color);
							}
						}
					}
				}
				else {
					// Compute visibility path (hierarchy traversal -> page emit -> cluster cull -> indirect visibility draw)
					RGHandle rg_visible_pages{};
					if (hierarchy_traversal_pass)
						rg_visible_pages = hierarchy_traversal_pass->add_to_graph(render_graph, scene_view, render_config, render_scene, meshes, visible_count, gpu_scene, current_idx);

					gpu_scene.ensure_hiz_textures(rhi, scene_view.viewport_width, scene_view.viewport_height);
					gpu_scene.ensure_color_textures(rhi, scene_view.viewport_width, scene_view.viewport_height);

					RGHandle rg_history_hiz{};
					if (gpu_scene.has_history_hiz() && render_config.enable_hiz_culling) {
						auto hist_tex = gpu_scene.get_history_hiz(current_idx);
						if (hist_tex.is_valid())
							rg_history_hiz = render_graph.import_texture("HistoryHiZ", hist_tex, ResourceState::ShaderResource);
					}

					RGHandle rg_history_color{};
					if (gpu_scene.has_history_color() && (render_config.enable_ssr || render_config.enable_ssgi)) {
						auto hist_col_tex = gpu_scene.get_history_color(current_idx);
						if (hist_col_tex.is_valid())
							rg_history_color = render_graph.import_texture("HistorySceneColor", hist_col_tex, ResourceState::ShaderResource);
					}

					RGHandle rg_dyn_instances{};
					if (render_config.enable_virtual_geometry && rg_visible_pages.is_valid()) {
						if (page_emit_pass)
							page_emit_pass->add_to_graph(render_graph, render_config, gpu_scene, current_idx);

						if (cluster_cull_pass)
							rg_dyn_instances = cluster_cull_pass->add_to_graph(render_graph, rg_history_hiz, rg_draw, scene_view, render_config, gpu_scene, current_idx);
					}

					// Visibility Indirect Pass: rasterize indirect draws into VisibilityBuffer + DepthBuffer
					if (visibility_pass && resolve_pass) {
						RGHandle rg_depth{};
						auto rg_visibility = visibility_pass->add_indirect_to_graph(render_graph, back_buffer, rg_depth,
							scene_view, render_config, render_scene, meshes, sort_list,
							ranges,
							rg_draw, rg_dyn_instances.is_valid() ? rg_dyn_instances : rg_instance_data, gpu_scene,
							gpu_scene.get_vertex_buffer(), gpu_scene.get_index_buffer(), &rg_depth);

						// Build Current Frame Hi-Z Pyramid from Depth Buffer
						RGHandle rg_current_hiz{};
						if (rg_depth.is_valid() && pyramid_mip_pass) {
							RGHandle target_hiz{};
							auto curr_tex = gpu_scene.get_current_hiz(current_idx);
							if (curr_tex.is_valid())
								target_hiz = render_graph.import_texture("CurrentHiZ", curr_tex, ResourceState::Undefined);

							rg_current_hiz = pyramid_mip_pass->add_to_graph(render_graph, rg_depth, render_config, target_hiz);
							if (rg_current_hiz.is_valid()) {
								gpu_scene.mark_history_hiz_valid();
								if (render_config.debug_hiz && pyramid_mip_debug_pass)
									pyramid_mip_debug_pass->add_to_graph(render_graph, back_buffer, rg_current_hiz, render_config.debug_hiz_mip);
							}
						}

						RGHandle rg_ao{};
						if (rg_depth.is_valid() && ao_pass && render_config.ao_mode != AOMode::Disabled) {
							RGHandle raw_ao = ao_pass->add_to_graph(render_graph, rg_depth, scene_view, render_config);
							if (raw_ao.is_valid() && ao_temporal_pass)
								raw_ao = ao_temporal_pass->add_to_graph(render_graph, raw_ao, rg_depth, scene_view, render_config);

							if (raw_ao.is_valid() && ao_blur_pass)
								rg_ao = ao_blur_pass->add_to_graph(render_graph, raw_ao, rg_depth, scene_view, render_config);
							else
								rg_ao = raw_ao;
						}

						RGHandle rg_ssr{};
						if (rg_depth.is_valid() && ssr_pass && render_config.enable_ssr)
							rg_ssr = ssr_pass->add_to_graph(render_graph, rg_depth, rg_history_color, scene_view, render_config);

						RGHandle rg_ssgi{};
						if (rg_depth.is_valid() && ssgi_pass && render_config.enable_ssgi)
							rg_ssgi = ssgi_pass->add_to_graph(render_graph, rg_depth, rg_history_color, scene_view, render_config);

						if (rg_visibility.is_valid()) {
							resolve_pass->add_to_graph(render_graph, back_buffer, rg_visibility,
								scene_view, render_config, gpu_scene, shadow_map, rg_ao, rg_ssr, rg_ssgi);
							has_main_pass = true;

							if (physics_debug_pass && render_config.debug_physics)
								physics_debug_pass->add_to_graph(render_graph, back_buffer, rg_depth,
									scene_view, render_config);

							if (forward_translucent_pass && ranges.range_c_count > 0)
								forward_translucent_pass->add_to_graph(render_graph, shadow_map, back_buffer, rg_depth,
									render_scene, scene_view, render_config, meshes, sort_list,
									ranges, rg_draw, rg_inst, gpu_scene,
									gpu_scene.get_vertex_buffer(), gpu_scene.get_index_buffer(),
									rg_ao, rg_ssr, rg_ssgi, rg_history_color);
						}
					}
				}

				if (has_main_pass && (render_config.enable_ssr || render_config.enable_ssgi)) {
					gpu_scene.mark_history_color_valid();
					auto curr_col_tex = gpu_scene.get_current_color(current_idx);
					if (curr_col_tex.is_valid()) {
						auto rg_curr_color = render_graph.import_texture("CurrentSceneColor", curr_col_tex, ResourceState::Undefined);
						render_graph.add_pass("Capture Scene Color",
							[=](RGBuilder& builder) {
								builder.read(back_buffer, ResourceState::TransferSrc);
								builder.write(rg_curr_color, ResourceState::TransferDst);
								return rg_curr_color;
							},
							[=](RHI* rhi, CommandHandle cmd) {
								rhi->cmd_copy_image(cmd, render_graph.get_texture(back_buffer), render_graph.get_texture(rg_curr_color));
								rhi->resource_barrier(cmd, render_graph.get_texture(rg_curr_color), ResourceState::TransferDst, ResourceState::ShaderResource);
							}
						);
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
		}
		else {
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

		render_graph.execute(cmd);

		CommandHandle active_cmd = rhi->get_current_graphics_command_buffer();
		if (rhi->is_headless()) {
			// Transition to TransferSrc
			rhi->resource_barrier(active_cmd, swapchain_tex, ResourceState::RenderTarget, ResourceState::TransferSrc);

			uint32_t current_idx = rhi->get_current_image_index();
			if (readback_buffers.size() <= current_idx) {
				readback_buffers.resize(current_idx + 1);
			}

			auto swapchain_desc = rhi->get_texture_desc(swapchain_tex);
			uint64_t readback_size = (uint64_t)swapchain_desc.width * swapchain_desc.height * 4; // Assuming RGBA8
			uint64_t current_buf_size = readback_buffers[current_idx].is_valid() ? rhi->get_buffer_desc(readback_buffers[current_idx]).size : 0;
			if (!readback_buffers[current_idx].is_valid() || current_buf_size != readback_size) {
				if (readback_buffers[current_idx].is_valid()) {
					rhi->destroy_buffer(readback_buffers[current_idx]);
				}
				// create read_back buffer
				readback_buffers[current_idx] = rhi->get_allocator()->alloc_read_back(readback_size);
			}

			// Perform copy
			rhi->cmd_copy_image_to_buffer(active_cmd, swapchain_tex, readback_buffers[current_idx]);
			// Barrier back to Present/Undefined doesn't strictly matter for offscreen, but we leave it as TransferSrc so it's clean next frame
		}
		else {
			rhi->resource_barrier(active_cmd, swapchain_tex, ResourceState::RenderTarget, ResourceState::Present);
		}

		auto& frames = gpu_scene.get_frame_resources();
		frames[current_idx].submit_timeline_value = rhi->get_graphics_timeline_value() + 1;
		frames[current_idx].requests_processed = false;

		rhi->end_frame(active_cmd);
		render_graph.reset();
	}



	void Renderer::set_config(const RenderConfig& config) {
		render_config = config;
	}

	void Renderer::update_physics_debug_vertices(const std::vector<PhysicsDebugVertex>& verts) {
		if (physics_debug_pass)
			physics_debug_pass->update_vertices(verts);
	}

	const RenderConfig& Renderer::get_config() const {
		return render_config;
	}

	const void* Renderer::get_readback_pixels() const {
		if (!rhi->is_headless() || readback_buffers.empty()) return nullptr;
		uint32_t current_idx = rhi->get_current_image_index();
		if (current_idx < readback_buffers.size() && readback_buffers[current_idx].is_valid()) {
			auto* buf = rhi->get_buffer(readback_buffers[current_idx]);
			return buf ? buf->mapped_ptr : nullptr;
		}
		return nullptr;
	}

	void Renderer::update_cascades(SceneView& view, const RenderConfig& config, const bud::math::AABB& scene_aabb) {
		auto cam_near = view.near_plane;
		auto cam_far = view.far_plane;
		if (!(cam_far > cam_near)) {
			cam_far = cam_near + 1.0f;	// guard: never allow a degenerate camera range
		}

		const uint32_t cascade_count = std::min(config.cascade_count, MAX_CASCADES);
		auto lambda = config.cascade_split_lambda;

		// Scene extent. Cascades never need to reach further than the world we have:
		// with shadow_far == cam_far == 500m in a ~60m scene, EVERY cascade box swallowed
		// the whole scene, all four maps rasterized the same geometry and CSM degenerated
		// into "4 copies of one blurry map" (shadows sliding with camera pitch).
		const bool has_scene_bounds = (scene_aabb.max.x >= scene_aabb.min.x);
		float scene_radius = has_scene_bounds ? bud::math::length(scene_aabb.max - scene_aabb.min) * 0.5f : 0.0f;

		float shadow_far = std::min(config.shadow_far_plane, cam_far);
		if (scene_radius > 0.0f && config.shadow_far_scene_factor > 0.0f) {
			shadow_far = std::min(shadow_far, scene_radius * config.shadow_far_scene_factor);
		}
		shadow_far = std::max(shadow_far, cam_near * 2.0f);

		// Hysteresis on the scene-derived reach. scene_bounds grow/shrink as virtual
		// geometry pages stream in and out; if shadow_far tracked that every frame, the
		// derived texel footprint of every cascade would wobble (observed ~0.4%/s), the
		// texel-snap grid would rescale under the camera and the shadows would creep
		// again. Only adopt a new reach once it moved by more than 10%.
		{
			// If shadow configuration parameters changed, invalidate cached reach
			if (config.shadow_far_plane != cached_shadow_far_plane ||
				config.shadow_far_scene_factor != cached_shadow_far_scene_factor) {
				cached_shadow_far_plane = config.shadow_far_plane;
				cached_shadow_far_scene_factor = config.shadow_far_scene_factor;
				cached_shadow_far = -1.0f;
			}

			const float derived = shadow_far;
			if (cached_shadow_far > 0.0f &&
				std::abs(derived - cached_shadow_far) < 0.1f * std::max(derived, 1.0f))
				shadow_far = cached_shadow_far;
			else
				cached_shadow_far = derived;
		}

		// 1. Calculate Split Depths (Log-Linear)
		// The classical formula uses cam_near as the logarithmic base. With a 1cm near
		// plane that base spans 5 orders of magnitude against shadow_far, the log term
		// collapses to ~0 and the mix degenerates to a purely uniform split. Clamp the
		// base to a small fraction of shadow_far so the near cascades really stay near.
		const float log_base = std::max(cam_near, shadow_far * 1e-3f);
		float cascade_splits[MAX_CASCADES];
		for (uint32_t i = 0; i < MAX_CASCADES; ++i) {
			cascade_splits[i] = 1.0f;
		}
		for (uint32_t i = 0; i < cascade_count; ++i) {
			auto p = (float)(i + 1) / (float)cascade_count;
			auto log = log_base * std::pow(shadow_far / log_base, p);
			auto uniform = cam_near + (shadow_far - cam_near) * p;
			auto d = lambda * log + (1.0f - lambda) * uniform;
			d = std::max(cam_near, std::min(d, cam_far));
			// Normalized lerp parameter between the near- and far-plane frustum corners.
			// Along a camera ray this IS linear in view depth, so it selects the slice.
			cascade_splits[i] = (d - cam_near) / (cam_far - cam_near);
			view.cascade_split_depths[i] = d;
		}

		for (uint32_t i = cascade_count; i < MAX_CASCADES; ++i) {
			view.cascade_split_depths[i] = view.far_plane;
			view.cascade_view_proj_matrices[i] = bud::math::mat4(1.0f);
			// texel_size == 0 doubles as the "cascade unused" sentinel that
			// lighting.glsl uses to skip a layer instead of sampling a non-existent
			// slice of the shadow-map array.
			view.cascade_texel_size[i] = 0.0f;
			view.cascade_depth_range[i] = 1.0f;
		}

		// 2. Calculate Matrices
		auto inv_cam_matrix = bud::math::inverse(view.proj_matrix * view.view_matrix);
		auto L = bud::math::normalize(view.light_dir);
		auto up = (std::abs(L.y) > 0.99f) ? bud::math::vec3(0.0f, 0.0f, 1.0f) : bud::math::vec3(0.0f, 1.0f, 0.0f);
		// Rotation-only light view: the eye stays at the world origin, so translating the
		// camera can never jitter the projection. Only the texel-snap below moves it.
		auto light_rot_matrix = bud::math::lookAt(bud::math::vec3(0.0f), -L, up);
		const float shadow_map_texels = (float)std::max<uint32_t>(config.shadow_map_size, 1u);
		// Casters that sit between the light and a slice but still fall onto it: a whole
		// scene diagonal is the tight upper bound (the slice itself may be outside the AABB).
		// Quantised onto a 4m ladder on purpose: scene_bounds grow and shrink as virtual
		// geometry pages stream in and out, and *which* pages stream depends on where the
		// camera is pointed, so an unquantised reach re-derived the cascade slab length
		// under camera motion - and that length is the metres-per-unit-depth the shadow
		// biases are expressed in (see the slab below). A step is rare; a drift is not.
		const float caster_reach = std::ceil(std::max(scene_radius * 2.0f, 1.0f) / 4.0f) * 4.0f;

		for (uint32_t i = 0; i < cascade_count; ++i) {
			const float prev_d = (i == 0) ? cam_near : view.cascade_split_depths[i - 1];
			const float curr_d = view.cascade_split_depths[i];
			const float slice_span = curr_d - prev_d;
			const float overlap = slice_span * 0.20f;

			float slice_near_d = cam_near;
			if (i > 0)
				slice_near_d = std::max(cam_near, prev_d - overlap);

			float slice_far_d = curr_d;
			if (i < cascade_count - 1)
				slice_far_d = std::min(cam_far, curr_d + overlap);

			const float slice_near_split = (slice_near_d - cam_near) / (cam_far - cam_near);
			const float slice_far_split = (slice_far_d - cam_near) / (cam_far - cam_near);

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

				frustum_corners[j] = bud::math::vec3(vec_near + (vec_far - vec_near) * slice_near_split);
				frustum_corners[j + 4] = bud::math::vec3(vec_near + (vec_far - vec_near) * slice_far_split);
			}

			// 3. 该级联切片的包围球（中心 + 半径）。两者对刚体变换都是不变的，
			//    所以 texel 物理尺寸只随 FOV / aspect / 切分深度变化，相机移动或俯仰
			//    时完全恒定 —— 这是消除"阴影随视角滑动"的前提。
			bud::math::vec3 frustum_center(0.0f);
			for (const auto& v : frustum_corners)
				frustum_center += v;

			frustum_center /= 8.0f;

			auto radius = 0.0f;
			for (const auto& v : frustum_corners)
				radius = std::max(radius, bud::math::length(v - frustum_center));

			// 包围球本身已经包住 8 个角点，ortho 半边长直接用它。
			radius = std::max(radius, 0.25f);

			// Hold the reach on an anchor so camera unprojection float jitter cannot
			// rescale world_units_per_texel every frame. If the required radius grows
			// larger than reach_anchor, expand immediately with 4% headroom so it never
			// clips the slice. If radius shrinks by > 10%, adopt the smaller anchor.
			float& reach_anchor = cascade_reach_anchor_[i];
			if (reach_anchor <= 0.0f || radius > reach_anchor || radius < reach_anchor * 0.90f)
				reach_anchor = radius * 1.04f;

			radius = reach_anchor;

			// 4. 固定的 texel 物理尺寸
			const float texel_size = (2.0f * radius) / shadow_map_texels;

			// 5. 光空间中心 + Texel Snapping（消除最后不到一个 texel 的抖动）
			auto center_of_light_space = light_rot_matrix * bud::math::vec4(frustum_center, 1.0f);
			float snapped_x = std::floor(center_of_light_space.x / texel_size) * texel_size;
			float snapped_y = std::floor(center_of_light_space.y / texel_size) * texel_size;

			float min_x = snapped_x - radius;
			float max_x = snapped_x + radius;
			float min_y = snapped_y - radius;
			float max_y = snapped_y + radius;

			// 6. 逐级联独立的深度 slab：基于外接球中心与半径推导。
			//    采用与 XY 正交盒完全一致的外接球方案，确保整个外接球及其接收面在
			//    光空间 Z 轴上也被 100% 完整囊括，彻底杜绝边界截断和漏光。
			const float center_d = -center_of_light_space.z;
			const float far_margin = std::max(radius * 0.20f, 2.0f);
			const float slab_length = caster_reach + 2.0f * radius + far_margin;
			float far_z = center_d + radius + far_margin;
			float near_z = center_d - radius - caster_reach;
			if (!(far_z > near_z + 1e-3f))
				near_z = far_z - 1.0f;	// guard: never emit a collapsed/inverted slab

			// 7. 构建最终矩阵
			auto light_proj_matrix = config.reversed_z
				? bud::math::ortho_vk_reversed(min_x, max_x, min_y, max_y, near_z, far_z)
				: bud::math::ortho_vk(min_x, max_x, min_y, max_y, near_z, far_z);

			view.cascade_view_proj_matrices[i] = light_proj_matrix * light_rot_matrix;
			// 导出给接收端 shader：bias 以 texel / 米为单位表达，不再依赖
			// "归一化深度"这种每级联含义都不同的魔数。
			view.cascade_texel_size[i] = texel_size;
			view.cascade_depth_range[i] = far_z - near_z;
		}

		// --- Throttled CSM diagnostic (<= 1 line set / second) -------------------
		// Lets you answer "are the cascades actually separated?" from the log instead
		// of guessing in Nsight. Before the cascade-sizing fix all four rows printed
		// nearly the same box, which is exactly the symptom of "4 shadow maps identical".
		// {
		// 	static float s_next_print = -1.0f;
		// 	if (view.time >= s_next_print) {
		// 		s_next_print = view.time + 1.0f;
		// 		bud::print("[CSM] cascades={} shadow_far={:.4f}m scene_radius={:.4f}m map={}px aspect={:.6f} fov={:.4f}",
		// 			cascade_count, shadow_far, scene_radius, config.shadow_map_size,
		// 			view.viewport_width / std::max(view.viewport_height, 1.0f), view.fov);
		// 		for (uint32_t i = 0; i < cascade_count; ++i) {
		// 			bud::print("[CSM]   C{}: view_depth<={:8.4f}m ortho_box={:9.4f}m texel={:9.6f}m z_slab={:9.4f}m",
		// 				i, view.cascade_split_depths[i],
		// 				view.cascade_texel_size[i] * (float)config.shadow_map_size,
		// 				view.cascade_texel_size[i], view.cascade_depth_range[i]);
		// 		}
		// 	}
		// }
	}

	void Renderer::select_occluders_cpu(const RenderScene& render_scene, const SceneView& view, const std::vector<SortItem>& source_list, size_t source_count, std::vector<SortItem>& out_occluders, size_t out_count) {
		out_occluders.clear();
		if (out_count == 0) return;

		size_t n = std::min<size_t>(source_count, source_list.size());
		std::vector<std::pair<float, size_t>> scores;
		scores.reserve(n);
		const float eps = 1e-6f;

		for (size_t i = 0; i < n; ++i) {
			const auto& item = source_list[i];
			if (item.entity_index == UINT32_MAX) continue;
			uint32_t entity_idx = item.entity_index;
			if (entity_idx >= render_scene.size()) continue;
			uint32_t mesh_id = render_scene.mesh_indices[entity_idx];
			if (mesh_id >= meshes.size()) continue;
			const auto& mesh = meshes[mesh_id];

			float radius = mesh.sphere.radius;
			uint32_t tri_count = mesh.index_count / 3;
			if (item.submesh_index != UINT32_MAX && item.submesh_index < mesh.submeshes.size()) {
				radius = mesh.submeshes[item.submesh_index].sphere.radius;
				tri_count = mesh.submeshes[item.submesh_index].index_count / 3;
			}

			auto world_matrix = render_scene.world_matrices[entity_idx];
			bud::math::vec3 pos = bud::math::vec3(world_matrix[3]);
			float dist2 = bud::math::distance2(pos, view.camera_position);

			float score = (radius * radius) / (dist2 + eps);
			score *= (1.0f + render_config.heuristic_occluder_tri_weight * static_cast<float>(tri_count));

			scores.emplace_back(score, i);
		}

		if (scores.empty()) return;

		size_t k = std::min(out_count, scores.size());
		// select top-k by score (descending)
		std::nth_element(scores.begin(), scores.begin() + k, scores.end(), [](auto& a, auto& b) { return a.first > b.first; });

		// sort top-k for stable order (largest first)
		std::sort(scores.begin(), scores.begin() + k, [](auto& a, auto& b) { return a.first > b.first; });

		out_occluders.reserve(k);
		uint32_t tris_sum = 0;
		for (size_t i = 0; i < k; ++i) {
			size_t src_idx = scores[i].second;
			out_occluders.push_back(source_list[src_idx]);
			const auto& it = source_list[src_idx];
			uint32_t ent = it.entity_index;
			if (ent < render_scene.size()) {
				uint32_t mid = render_scene.mesh_indices[ent];
				if (mid < meshes.size()) {
					const auto& m = meshes[mid];
					if (it.submesh_index != UINT32_MAX && it.submesh_index < m.submeshes.size()) tris_sum += m.submeshes[it.submesh_index].index_count / 3;
					else tris_sum += m.index_count / 3;
				}
			}
		}

		// Update runtime stats
		rhi->get_render_stats().occluder_count = static_cast<uint32_t>(out_occluders.size());
		rhi->get_render_stats().occluder_triangles = tris_sum;
	}
}
