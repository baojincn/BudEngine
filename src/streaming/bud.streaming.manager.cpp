#include "src/streaming/bud.streaming.manager.hpp"
#include "src/core/bud.logger.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include "src/physics/bud.cloth.hpp"
#include <algorithm>
#include <cfloat>
#include <cstring>

namespace bud::streaming {

StreamingManager::StreamingManager(bud::io::AssetManager* asset_manager,
	bud::graphics::GPUScene* gpu_scene,
	bud::graphics::Renderer* renderer,
	bud::graphics::RHI* rhi)
	: asset_manager(asset_manager), gpu_scene(gpu_scene), renderer(renderer), rhi(rhi)
{
}

StreamingManager::~StreamingManager() {
	if (alive_flag) {
		alive_flag->store(false, std::memory_order_release);
	}
}

void StreamingManager::register_virtual_geometry_async(const std::string& path) {
	{
		std::scoped_lock lock(mutex_sm);
		auto it = registered_info_map.find(path);
		if (it != registered_info_map.end()) {
			if (asset_registered_callback) {
				const auto& info = it->second;
				asset_registered_callback(path, info.mesh_id, info.global_aabb, info.root_group_index, info.base_virtual_page);
			}
			return;
		}
	}

	if (bud::physics::is_cloth_name_or_path(path)) {
		if (non_vg_asset_callback)
			non_vg_asset_callback(path);
		return;
	}

	auto alive = alive_flag;
	asset_manager->load_file_async(path, [this, alive, path](std::vector<char> data) {
		if (!alive->load(std::memory_order_acquire))
			return;
		if (data.empty()) {
			bud::eprint("[Streaming] Empty asset data for: {}", path);
			// Fall through to the non-VG path so the caller can attempt a
			// traditional mesh load (or simply complete its pending count).
			if (non_vg_asset_callback) {
				non_vg_asset_callback(path);
			}
			return;
		}

		const char* base = data.data();
		uint64_t page_data_base_offset = 0;
		bool has_vg_chunk = false;

		if (data.size() >= sizeof(bud::asset::BudAssetHeader)) {
			const auto* asset_header = reinterpret_cast<const bud::asset::BudAssetHeader*>(data.data());
			if (asset_header->magic == bud::asset::BUD_ASSET_MAGIC) {
				if (asset_header->chunk_table_offset + asset_header->chunk_count * sizeof(bud::asset::AssetChunkEntry) <= data.size()) {
					const auto* chunks = reinterpret_cast<const bud::asset::AssetChunkEntry*>(data.data() + asset_header->chunk_table_offset);
					for (uint32_t c = 0; c < asset_header->chunk_count; ++c) {
						if (chunks[c].chunk_type == static_cast<uint32_t>(bud::asset::AssetChunkType::VirtualGeometry)) {
							base = data.data() + chunks[c].offset;
							page_data_base_offset = chunks[c].offset;
							has_vg_chunk = true;
							break;
						}
					}
				}
			}
		}

		if (!has_vg_chunk) {
			// Standard / translucent mesh asset without Virtual Geometry DAG.
			// Report it so the caller can load it via the traditional mesh path.
			if (non_vg_asset_callback) {
				non_vg_asset_callback(path);
			}
			return;
		}

		if (reinterpret_cast<const uintptr_t>(base) + sizeof(bud::asset::VGHeader) > reinterpret_cast<const uintptr_t>(data.data() + data.size())) {
			bud::eprint("[Streaming] Invalid Virtual Geometry asset (too small): {}", path);
			return;
		}

		const auto* h = reinterpret_cast<const bud::asset::VGHeader*>(base);
		if (h->magic != bud::asset::VG_MAGIC) {
			bud::eprint("[Streaming] Bad Virtual Geometry magic 0x{:X} for: {}", h->magic, path);
			return;
		}
		if (h->version != bud::asset::VG_VERSION) {
			bud::eprint("[Streaming] Unsupported Virtual Geometry version {} for: {} (expected {})",
				h->version, path, bud::asset::VG_VERSION);
			return;
		}

		VirtualGeometryAsset asset;
		asset.path = path;
		asset.page_data_offset = page_data_base_offset + h->page_data_offset;
		asset.root_group_index = h->root_group_index;
		const auto* cluster_ptr = reinterpret_cast<const bud::asset::VGCluster*>(base + h->cluster_offset);
		asset.clusters.assign(cluster_ptr, cluster_ptr + h->cluster_count);
		const auto* group_ptr = reinterpret_cast<const bud::asset::VGClusterGroup*>(base + h->group_offset);
		asset.groups.assign(group_ptr, group_ptr + h->group_count);
		const auto* page_ptr = reinterpret_cast<const bud::asset::VGPageStreamingState*>(base + h->page_state_offset);
		asset.pages.assign(page_ptr, page_ptr + h->page_count);
		if (h->material_count > 0 && h->material_offset > 0) {
			const auto* mat_ptr = reinterpret_cast<const bud::asset::MaterialDescriptor*>(base + h->material_offset);
			asset.materials.assign(mat_ptr, mat_ptr + h->material_count);
		}
		if (h->texture_count > 0 && h->texture_offset > 0) {
			const char* p = base + h->texture_offset;
			for (uint32_t t = 0; t < h->texture_count; ++t) {
				asset.textures.push_back(std::string(p));
				p += asset.textures.back().size() + 1;
			}
		}

		// Register materials into GPUScene using asset-driven material descriptors.
		// The first material slot is stored as base_material_id so PageSubMesh entries
		// can reference the correct bindless texture slot at render time.
		for (size_t mi = 0; mi < asset.materials.size(); ++mi) {
			const auto& mat_desc = asset.materials[mi];
			bud::graphics::GPUMaterialData gpu_mat;
			gpu_mat.alpha_mode = static_cast<uint32_t>(mat_desc.alpha_mode);
			gpu_mat.alpha_cutoff = (mat_desc.alpha_cutoff > 0.0f) ? mat_desc.alpha_cutoff : 0.5f;
			gpu_mat.base_color_factor = glm::vec4(1.0f);
			gpu_mat.metallic_factor = mat_desc.metallic_factor;
			gpu_mat.roughness_factor = mat_desc.roughness_factor;

			auto resolve_and_bind_texture = [&](uint32_t tex_idx) -> uint32_t {
				if (tex_idx >= asset.textures.size() || !renderer) return 0;
				const std::string& tex_path = asset.textures[tex_idx];
				if (tex_path.empty()) return 0;
				return renderer->bind_texture_async(tex_path);
			};

			gpu_mat.albedo_texture_id = resolve_and_bind_texture(mat_desc.base_color_texture);
			gpu_mat.normal_texture_id = resolve_and_bind_texture(mat_desc.normal_texture);
			gpu_mat.metallic_roughness_id = resolve_and_bind_texture(mat_desc.metallic_roughness_texture);
			gpu_mat.emissive_texture_id = resolve_and_bind_texture(mat_desc.emissive_texture);

			if (gpu_scene) {
				uint32_t slot_id = gpu_scene->register_material(gpu_mat);
				// Record the first material's GPU slot as the base for this asset.
				if (mi == 0)
					asset.base_material_id = slot_id;
			}
		}

		// Per-page cluster range: derive from each cluster's position_page_offset
		// (pages are contiguous in the cluster table under greedy DAG packing).
		asset.page_cluster_start.assign(asset.pages.size(), 0);
		asset.page_cluster_count.assign(asset.pages.size(), 0);
		for (uint32_t ci = 0; ci < asset.clusters.size(); ++ci) {
			const uint32_t p = asset.clusters[ci].position_page_offset;
			if (p >= asset.pages.size())
				continue;
			if (asset.page_cluster_count[p] == 0)
				asset.page_cluster_start[p] = ci;
			asset.page_cluster_count[p]++;
		}

		// Per-page world AABB from the cluster position bounds (the file header
		// only carries a global AABB; distance-based eviction needs per-page).
		asset.page_aabbs.resize(asset.pages.size());
		for (uint32_t i = 0; i < asset.pages.size(); ++i) {
			bud::math::AABB aabb;
			const uint32_t cs = asset.page_cluster_start[i];
			for (uint32_t k = 0; k < asset.page_cluster_count[i]; ++k) {
				const auto& c = asset.clusters[cs + k];
				bud::math::vec3 cmin(
					c.position_bounds_center[0] - c.position_bounds_extent[0],
					c.position_bounds_center[1] - c.position_bounds_extent[1],
					c.position_bounds_center[2] - c.position_bounds_extent[2]);
				bud::math::vec3 cmax(
					c.position_bounds_center[0] + c.position_bounds_extent[0],
					c.position_bounds_center[1] + c.position_bounds_extent[1],
					c.position_bounds_center[2] + c.position_bounds_extent[2]);
				aabb.merge(cmin);
				aabb.merge(cmax);
			}
			asset.page_aabbs[i] = aabb;
		}

		bud::math::AABB global_aabb;
		global_aabb.min = bud::math::vec3(FLT_MAX, FLT_MAX, FLT_MAX);
		global_aabb.max = bud::math::vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (const auto& aabb : asset.page_aabbs) {
			global_aabb.merge(aabb.min);
			global_aabb.merge(aabb.max);
		}

		auto asset_ptr = std::make_shared<VirtualGeometryAsset>(std::move(asset));
		std::vector<uint32_t> initial_page_indices;
		{
			std::scoped_lock lock(mutex_sm);
			// Page raw data region is sequential: file offset = page_data_offset
			// + cumulative sizes.
			uint64_t off = asset_ptr->page_data_offset;
			std::vector<StreamingPage> temp_pages;
			temp_pages.reserve(asset_ptr->pages.size());
			uint32_t base_virtual_page = 0;
			uint32_t root_group_index = 0;
			if (gpu_scene) {
				base_virtual_page = gpu_scene->get_vg_pool().allocate_virtual_pages(static_cast<uint32_t>(asset_ptr->pages.size()));
				{
					std::scoped_lock vpk_lock(vpk_mutex);
					if (virtual_page_keys.size() < base_virtual_page + asset_ptr->pages.size()) {
						virtual_page_keys.resize(base_virtual_page + asset_ptr->pages.size());
					}
					for (uint32_t i = 0; i < asset_ptr->pages.size(); ++i) {
						virtual_page_keys[base_virtual_page + i] = path + ":page_" + std::to_string(i);
					}
				}

				// Allocate and upload groups
				root_group_index = gpu_scene->get_vg_pool().allocate_groups(static_cast<uint32_t>(asset_ptr->groups.size()));
				if (!asset_ptr->groups.empty()) {
					std::vector<bud::asset::VGClusterGroup> patched_groups = asset_ptr->groups;
					for (auto& g : patched_groups) {
						g.page_index_start += base_virtual_page;
						if (g.children_num > 0) {
							g.children_start += root_group_index;
						}
					}
					auto* buf = rhi ? rhi->get_buffer(gpu_scene->get_vg_pool().group_buffer) : nullptr;
					auto* group_mapped = buf ? static_cast<bud::asset::VGClusterGroup*>(buf->mapped_ptr) : nullptr;
					if (group_mapped) {
						std::memcpy(group_mapped + root_group_index, patched_groups.data(), patched_groups.size() * sizeof(bud::asset::VGClusterGroup));
					}
				}
			}
			std::string bulk_bin_path = std::filesystem::path(path).replace_extension(".budbulk").generic_string();
			for (uint32_t i = 0; i < asset_ptr->pages.size(); ++i) {
				StreamingPage sp;
				sp.asset_id = path;
				sp.page_id = i;
				sp.virtual_page_index = base_virtual_page + i;
				sp.file_offset = asset_ptr->pages[i].raw_vertex_offset;
				sp.capacity = asset_ptr->pages[i].size_in_bytes;
				sp.bin_path = bulk_bin_path;
				sp.is_virtual_geometry = true;
				sp.aabb = asset_ptr->page_aabbs[i];
				sp.global_aabb = global_aabb;
				sp.has_aabb = true;
				sp.dependency_page_id = asset_ptr->pages[i].dependency_page_id;
				sp.is_root = (asset_ptr->pages[i].dependency_page_id == bud::asset::INVALID_INDEX);

				temp_pages.push_back(std::move(sp));
			}

			if (asset_ptr->root_group_index < asset_ptr->groups.size()) {
				const auto& root_grp = asset_ptr->groups[asset_ptr->root_group_index];
				for (uint32_t p = 0; p < root_grp.page_index_num; ++p) {
					uint32_t page_local_idx = root_grp.page_index_start + p;
					if (page_local_idx < temp_pages.size()) {
						temp_pages[page_local_idx].is_root = true;
					}
				}
			}

			initial_page_indices.clear();
			for (const auto& sp : temp_pages) {
				all_pages.emplace(sp.get_unique_id(), sp);
				// Automatically preload all pages if asset fits within pool capacity, ensuring complete shadow casters
				if (temp_pages.size() <= 4000 || sp.is_root) {
					initial_page_indices.push_back(sp.virtual_page_index);
				}
			}

			virtual_geometry_assets[path] = asset_ptr;

			//bud::print("[Streaming] Registered Virtual Geometry asset: {} ({} pages, {} clusters, {} groups, bulk={})",
			//	path, asset_ptr->pages.size(),
			//	asset_ptr->clusters.size(),
			//	asset_ptr->groups.size(),
			//	bulk_bin_path);

			if (asset_registered_callback) {
				std::vector<bud::graphics::PageSubMesh> page_submeshes;
				page_submeshes.reserve(asset_ptr->pages.size());
				uint32_t total_indices = 0;
				for (uint32_t pi = 0; pi < asset_ptr->pages.size(); ++pi) {
					bud::graphics::PageSubMesh psub{};
					psub.index_start = asset_ptr->pages[pi].raw_index_offset / 2u;
					psub.index_count = asset_ptr->pages[pi].raw_index_count * 3u;
					psub.page_index = base_virtual_page + pi;
					psub.aabb = asset_ptr->page_aabbs[pi];
					psub.lod_level = asset_ptr->pages[pi].flags;
					psub.material_id = asset_ptr->base_material_id;
					page_submeshes.push_back(psub);
					total_indices += psub.index_count;
				}

				float dummy_errs[3] = {0.0f, FLT_MAX, FLT_MAX};
				uint32_t mesh_id = renderer->register_page_based_mesh(
					base_virtual_page,
					static_cast<uint32_t>(asset_ptr->clusters.size()),
					total_indices,
					global_aabb,
					global_aabb,
					0,
					0,
					page_submeshes,
					{},
					dummy_errs);

				RegisteredVGAssetInfo rinfo;
				rinfo.mesh_id = mesh_id;
				rinfo.global_aabb = global_aabb;
				rinfo.root_group_index = root_group_index + asset_ptr->root_group_index;
				rinfo.base_virtual_page = base_virtual_page;
				registered_info_map[path] = rinfo;

				asset_registered_callback(path, mesh_id, global_aabb, rinfo.root_group_index, base_virtual_page);
			}
		}

		// Preload virtual pages immediately (outside mutex lock)
		if (!initial_page_indices.empty()) {
			process_gpu_page_requests(initial_page_indices.data(), static_cast<uint32_t>(initial_page_indices.size()));
		}
	});
}

void StreamingManager::update(const bud::math::vec3& camera_position) {
	// Step 0: Increment frame counter and update access records for resident pages
	{
		std::scoped_lock alock(access_mutex);
		++current_frame_number;
		// Update access record for each resident page (access = visible this frame)
		std::scoped_lock lock(mutex_sm);
		for (const auto& [page_key, resident] : residency_sm) {
			if (!resident) continue;
			auto it = all_pages.find(page_key);
			if (it == all_pages.end()) continue;
			const auto& sp = it->second;
			bud::math::vec3 bmin = sp.has_aabb ? sp.aabb.min : sp.global_aabb.min;
			bud::math::vec3 bmax = sp.has_aabb ? sp.aabb.max : sp.global_aabb.max;
			bud::math::vec3 closest(
				std::clamp(camera_position.x, bmin.x, bmax.x),
				std::clamp(camera_position.y, bmin.y, bmax.y),
				std::clamp(camera_position.z, bmin.z, bmax.z));
			float d = bud::math::length(camera_position - closest);
			page_access_records[page_key] = { current_frame_number, d };
		}
	}
	// Step 1: Process retry queue from previous frame (outside mutex_sm to avoid deadlock)
	std::vector<std::string> retry_copy;
	{
		std::scoped_lock rlock(retry_mutex);
		retry_copy.swap(retry_queue);
	}
	if (!retry_copy.empty()) {
		process_gpu_page_requests_from_keys(retry_copy);
	}

	// Step 2: Distance-based eviction
	std::scoped_lock lock(mutex_sm);
	for (const auto& [page_key, sp] : all_pages) {
		// Per-page distance-based eviction: distance from the camera to the
		// page's own AABB. Falls back to the asset AABB when a page has no AABB
		// so nothing accidentally unloads the whole scene.
		bud::math::vec3 bmin, bmax;
		if (sp.has_aabb) {
			bmin = sp.aabb.min;
			bmax = sp.aabb.max;
		} else {
			bmin = sp.global_aabb.min;
			bmax = sp.global_aabb.max;
		}
		bud::math::vec3 closest(
			std::clamp(camera_position.x, bmin.x, bmax.x),
			std::clamp(camera_position.y, bmin.y, bmax.y),
			std::clamp(camera_position.z, bmin.z, bmax.z));
		const float d = bud::math::length(camera_position - closest);
		const bool resident = residency_sm.count(page_key) && residency_sm[page_key];
		bool recently_accessed = false;
		if (auto rec_it = page_access_records.find(page_key); rec_it != page_access_records.end()) {
			if (current_frame_number - rec_it->second.last_access_frame < 60) {
				recently_accessed = true;
			}
		}
		if (resident && !sp.is_root && d > unload_radius_ && !recently_accessed) {
			if (auto it = page_gpu_slots.find(page_key); it != page_gpu_slots.end()) {
				gpu_scene->update_page_table_entry(sp.virtual_page_index, 0, 0);
				gpu_scene->get_page_pool().free_page(it->second);
				page_gpu_slots.erase(it);
			}
			pending_loads.erase(page_key);
			residency_sm.erase(page_key);
		}
	}
}

bool StreamingManager::is_page_resident(const std::string& page_key) const {
	std::scoped_lock lock(mutex_sm);
	auto it = residency_sm.find(page_key);
	return it != residency_sm.end() && it->second;
}

void StreamingManager::process_gpu_page_requests(const uint32_t* virtual_page_indices, uint32_t count) {
	std::vector<std::string> to_load;
	{
		std::scoped_lock lock(mutex_sm);
		std::scoped_lock vpk_lock(vpk_mutex);
		for (uint32_t i = 0; i < count; ++i) {
			const uint32_t vpi = virtual_page_indices[i];
			if (vpi >= virtual_page_keys.size())
				continue;
			const std::string& page_key = virtual_page_keys[vpi];
			if (page_key.empty() || pending_loads.count(page_key))
				continue;
			if (auto it = residency_sm.find(page_key); it != residency_sm.end() && it->second)
				continue;
			to_load.push_back(page_key);
		}
	}

	for (const auto& p : to_load) {
		std::scoped_lock lock(mutex_sm);
		if (auto it = residency_sm.find(p); it != residency_sm.end() && it->second)
			continue;
		if (pending_loads.count(p))
			continue;
		auto page_it = all_pages.find(p);
		if (page_it == all_pages.end())
			continue;

		residency_sm[p] = false;
		pending_loads.insert(p);

		const StreamingPage& sp = page_it->second;
		asset_manager->load_file_chunk_async(sp.bin_path, sp.file_offset, sp.capacity,
			[this, p, sp](std::vector<char> data) {
				if (data.empty()) {
					std::scoped_lock lock(mutex_sm);
					pending_loads.erase(p);
					return;
				}

				if (sp.is_virtual_geometry) {
					if (data.size() < sizeof(bud::asset::VGPageDataHeader)) {
						std::scoped_lock lock(mutex_sm);
						pending_loads.erase(p);
						bud::eprint("[Streaming] Virtual Geometry raw page chunk too small: {}", p);
						return;
					}
					const auto& ph = *reinterpret_cast<const bud::asset::VGPageDataHeader*>(data.data());
					if (ph.magic != bud::asset::VG_PAGE_DATA_MAGIC) {
						std::scoped_lock lock(mutex_sm);
						pending_loads.erase(p);
						bud::eprint("[Streaming] Bad Virtual Geometry page magic 0x{:X} for: {}", ph.magic, p);
						return;
					}
				}

				uint32_t slot = gpu_scene->get_page_pool().allocate_page();
				if (slot == ~0u) {
					bud::print("[Streaming] Page pool exhausted for: {} — adding to retry queue and evicting furthest pages", p);
					// Add to retry queue for the next frame
					{
						std::scoped_lock rlock(retry_mutex);
						if (retry_queue.size() < max_retry_queue_size)
							retry_queue.push_back(p);
					}
					// Trigger eviction to free up slots
					// (camera position is approximated from the page's AABB center)
					bud::math::vec3 cam_pos_approx = (sp.aabb.min + sp.aabb.max) * 0.5f;
					evict_furthest_pages(max_evict_per_frame, cam_pos_approx);
					{
						std::scoped_lock lock(mutex_sm);
						pending_loads.erase(p);
					}
					return;
				}
				uint32_t gpu_offset = gpu_scene->get_page_pool().get_page_offset(slot);

				// Copy data to page pool via staging upload
				const size_t src_size = data.size();
				if (src_size > bud::graphics::GPUScene::PagePool::page_size) {
					bud::eprint("[Streaming] Page {} is larger than pool slot ({} > {})", p, src_size, bud::graphics::GPUScene::PagePool::page_size);
					gpu_scene->get_page_pool().free_page(slot);
					return;
				}

				// 将数据移入 lambda，在 render thread 的 flush_upload_queue() 中执行 staging 分配与 GPU copy
				auto page_pool_buf = gpu_scene->get_page_pool_buffer();
				renderer->enqueue_rhi_command([this, p, sp, slot, gpu_offset, page_pool_buf, data = std::move(data)]() {
					const size_t src_size = data.size();
					auto staging = rhi->get_allocator()->alloc_staging(src_size);
					if (staging.mapped_ptr) {
						std::memcpy(staging.mapped_ptr, data.data(), src_size);
						rhi->copy_buffer_immediate_offset(staging.buffer, page_pool_buf, src_size, staging.offset, gpu_offset);
					}

					if (sp.is_virtual_geometry)
						gpu_scene->update_page_table_entry(sp.virtual_page_index, slot, 1);

					{
						std::scoped_lock lock(mutex_sm);
						residency_sm[p] = true;
						pending_loads.erase(p);
						page_gpu_slots[p] = slot;
					}
				});
			});
	}
}

void StreamingManager::evict_furthest_pages(uint32_t count, const bud::math::vec3& camera_position) {
	// Collect all resident pages with their LRU score (lower = better to evict)
	struct Candidate {
		float score;
		std::string page_key;
	};
	std::vector<Candidate> candidates;
	{
		std::scoped_lock lock(mutex_sm);
		for (const auto& [page_key, resident] : residency_sm) {
			if (!resident) continue;
			auto it = all_pages.find(page_key);
			if (it == all_pages.end()) continue;
			const auto& sp = it->second;
			if (sp.is_root) continue; // Coarse LOD root pages are pinned permanently in memory
			bud::math::vec3 bmin = sp.has_aabb ? sp.aabb.min : sp.global_aabb.min;
			bud::math::vec3 bmax = sp.has_aabb ? sp.aabb.max : sp.global_aabb.max;
			bud::math::vec3 closest(
				std::clamp(camera_position.x, bmin.x, bmax.x),
				std::clamp(camera_position.y, bmin.y, bmax.y),
				std::clamp(camera_position.z, bmin.z, bmax.z));
			float d = bud::math::length(camera_position - closest);

			// LRU score: distance weighted by recency
			// Pages not in access_records get a default score (oldest priority)
			std::scoped_lock alock(access_mutex);
			float recency_weight = 0.3f;
			auto rec_it = page_access_records.find(page_key);
			uint64_t age = (rec_it != page_access_records.end())
				? (current_frame_number - rec_it->second.last_access_frame)
				: UINT64_MAX;
			float age_norm = std::min(1.0f, static_cast<float>(age) / 1000.0f);
			float score = d * (1.0f - recency_weight) + age_norm * 1000.0f * recency_weight;
			candidates.push_back({ score, page_key });
		}
	}

	// Sort by score (highest first = best to evict)
	std::sort(candidates.begin(), candidates.end(),
		[](const auto& a, const auto& b) { return a.score > b.score; });

	// Evict the highest-scored 'count' pages
	uint32_t evicted = 0;
	for (const auto& cand : candidates) {
		if (evicted >= count) break;
		const auto& page_key = cand.page_key;
		std::scoped_lock lock(mutex_sm);
		auto it = all_pages.find(page_key);
		if (it == all_pages.end()) continue;
		const auto& sp = it->second;
		if (auto slot_it = page_gpu_slots.find(page_key); slot_it != page_gpu_slots.end()) {
			gpu_scene->update_page_table_entry(sp.virtual_page_index, 0, 0);
			gpu_scene->get_page_pool().free_page(slot_it->second);
			page_gpu_slots.erase(slot_it);
		}
		pending_loads.erase(page_key);
		residency_sm.erase(page_key);
		++evicted;
	}
	if (evicted > 0) {
		bud::print("[Streaming] Evicted {} pages (LRU + distance)", evicted);
	}
}

void StreamingManager::process_gpu_page_requests_from_keys(const std::vector<std::string>& page_keys) {
	std::vector<std::string> to_load;
	{
		std::scoped_lock lock(mutex_sm);
		for (const auto& p : page_keys) {
			if (pending_loads.count(p) || p.empty()) continue;
			if (auto it = residency_sm.find(p); it != residency_sm.end() && it->second) continue;
			to_load.push_back(p);
		}
	}

	for (const auto& p : to_load) {
		std::scoped_lock lock(mutex_sm);
		if (pending_loads.count(p)) continue;
		if (auto it = residency_sm.find(p); it != residency_sm.end() && it->second) continue;
		auto page_it = all_pages.find(p);
		if (page_it == all_pages.end()) continue;
		residency_sm[p] = false;
		pending_loads.insert(p);

		const StreamingPage& sp = page_it->second;
		auto alive = alive_flag;
		asset_manager->load_file_chunk_async(sp.bin_path, sp.file_offset, sp.capacity,
			[this, alive, p, sp](std::vector<char> data) {
				if (!alive->load(std::memory_order_acquire))
					return;
				if (data.empty()) {
					std::scoped_lock lock(mutex_sm);
					pending_loads.erase(p);
					return;
				}

				if (sp.is_virtual_geometry) {
					if (data.size() < sizeof(bud::asset::VGPageDataHeader)) {
						std::scoped_lock lock(mutex_sm);
						pending_loads.erase(p);
						bud::eprint("[Streaming] Virtual Geometry raw page chunk too small: {}", p);
						return;
					}
					const auto& ph = *reinterpret_cast<const bud::asset::VGPageDataHeader*>(data.data());
					if (ph.magic != bud::asset::VG_PAGE_DATA_MAGIC) {
						std::scoped_lock lock(mutex_sm);
						pending_loads.erase(p);
						bud::eprint("[Streaming] Bad Virtual Geometry page magic 0x{:X} for: {}", ph.magic, p);
						return;
					}
				}

				uint32_t slot = gpu_scene->get_page_pool().allocate_page();
				if (slot == ~0u) {
					bud::print("[Streaming] Page pool exhausted for: {} (retry) — adding to retry queue", p);
					{
						std::scoped_lock rlock(retry_mutex);
						if (retry_queue.size() < max_retry_queue_size)
							retry_queue.push_back(p);
					}
					bud::math::vec3 cam_pos_approx = (sp.aabb.min + sp.aabb.max) * 0.5f;
					evict_furthest_pages(max_evict_per_frame, cam_pos_approx);
					{
						std::scoped_lock lock(mutex_sm);
						pending_loads.erase(p);
					}
					return;
				}
				uint32_t gpu_offset = gpu_scene->get_page_pool().get_page_offset(slot);

				const size_t src_size = data.size();
				if (src_size > bud::graphics::GPUScene::PagePool::page_size) {
					bud::eprint("[Streaming] Page {} is larger than pool slot ({} > {})", p, src_size, bud::graphics::GPUScene::PagePool::page_size);
					gpu_scene->get_page_pool().free_page(slot);
					return;
				}

				auto page_pool_buf = gpu_scene->get_page_pool_buffer();
				renderer->enqueue_rhi_command([this, alive, p, sp, slot, gpu_offset, page_pool_buf, data = std::move(data)]() {
					if (!alive->load(std::memory_order_acquire))
						return;
					const size_t src_size = data.size();
					auto staging = rhi->get_allocator()->alloc_staging(src_size);
					if (staging.mapped_ptr) {
						std::memcpy(staging.mapped_ptr, data.data(), src_size);
						rhi->copy_buffer_immediate_offset(staging.buffer, page_pool_buf, src_size, staging.offset, gpu_offset);
					}

					if (sp.is_virtual_geometry)
						gpu_scene->update_page_table_entry(sp.virtual_page_index, slot, 1);

					{
						std::scoped_lock lock(mutex_sm);
						residency_sm[p] = true;
						pending_loads.erase(p);
						page_gpu_slots[p] = slot;
					}
				});
			});
	}
}

void StreamingManager::unregister_virtual_geometry(const std::string& path) {
	std::scoped_lock lock(mutex_sm);
	std::scoped_lock vpk_lock(vpk_mutex);

	// Find and remove all pages belonging to this asset
	std::vector<std::string> keys_to_remove;
	for (const auto& [page_key, sp] : all_pages) {
		if (sp.asset_id == path) {
			keys_to_remove.push_back(page_key);
		}
	}

	for (const auto& page_key : keys_to_remove) {
		auto it = all_pages.find(page_key);
		if (it == all_pages.end()) continue;
		uint32_t vpi = it->second.virtual_page_index;
		// Free GPU slot if allocated
		if (auto slot_it = page_gpu_slots.find(page_key); slot_it != page_gpu_slots.end()) {
			gpu_scene->update_page_table_entry(vpi, 0, 0);
			gpu_scene->get_page_pool().free_page(slot_it->second);
			page_gpu_slots.erase(slot_it);
		}
		// Clear virtual_page_keys entry
		if (vpi < virtual_page_keys.size())
			virtual_page_keys[vpi].clear();
		pending_loads.erase(page_key);
		residency_sm.erase(page_key);
		all_pages.erase(it);
	}

	// Remove the asset from the virtual_geometry_assets map
	virtual_geometry_assets.erase(path);

	//bud::print("[Streaming] Unregistered Virtual Geometry asset: {}", path);
}

} // namespace bud::streaming
