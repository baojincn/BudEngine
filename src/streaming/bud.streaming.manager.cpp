#include "src/streaming/bud.streaming.manager.hpp"
#include "src/core/bud.logger.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cfloat>
#include <cstring>

namespace {

// Cluster -> LOD level via the hierarchy level ranges (levels are contiguous
// cluster ranges appended LOD0 first).
uint32_t budnanite_cluster_level(const bud::asset::NaniteHierarchyLevel* levels, uint32_t level_count, uint32_t cluster_index) {
	for (uint32_t l = 0; l < level_count; ++l) {
		const auto& lv = levels[l];
		if (cluster_index >= lv.cluster_start && cluster_index < lv.cluster_start + lv.cluster_count)
			return l;
	}
	return 0;
}

} // namespace

namespace bud::streaming {

StreamingManager::StreamingManager(bud::io::AssetManager* asset_manager,
	bud::graphics::GPUScene* gpu_scene,
	bud::graphics::Renderer* renderer,
	bud::graphics::RHI* rhi)
	: asset_manager_(asset_manager), gpu_scene_(gpu_scene), renderer_(renderer), rhi_(rhi)
{
}

void StreamingManager::register_region(const RegionManifest& region) {
	std::scoped_lock lock(mutex_);
	regions_.push_back(region);
}

void StreamingManager::register_budnanite_async(const std::string& path) {
	asset_manager_->load_file_async(path, [this, path](std::vector<char> data) {
		if (data.size() < sizeof(bud::asset::NaniteHeader)) {
			bud::eprint("[Streaming] Invalid .budmesh (UE5-aligned layout, too small): {}", path);
			return;
		}
		const auto* h = reinterpret_cast<const bud::asset::NaniteHeader*>(data.data());
		if (h->magic != bud::asset::NANITE_MAGIC) {
			bud::eprint("[Streaming] Bad .budmesh magic 0x{:X} (expected BNNT layout) for: {}", h->magic, path);
			return;
		}
		if (h->version != bud::asset::NANITE_VERSION) {
			bud::eprint("[Streaming] Unsupported .budmesh version {} for: {} (expected {})",
				h->version, path, bud::asset::NANITE_VERSION);
			return;
		}

		BudNaniteAsset asset;
		asset.path = path;
		asset.page_data_offset = h->page_data_offset;
		const auto* base = data.data();
		const auto* cluster_ptr = reinterpret_cast<const bud::asset::NaniteCluster*>(base + h->cluster_offset);
		asset.clusters.assign(cluster_ptr, cluster_ptr + h->cluster_count);
		const auto* group_ptr = reinterpret_cast<const bud::asset::NaniteClusterGroup*>(base + h->group_offset);
		asset.groups.assign(group_ptr, group_ptr + h->group_count);
		const auto* level_ptr = reinterpret_cast<const bud::asset::NaniteHierarchyLevel*>(base + h->hierarchy_offset);
		asset.levels.assign(level_ptr, level_ptr + h->hierarchy_level_count);
		const auto* page_ptr = reinterpret_cast<const bud::asset::NanitePageStreamingState*>(base + h->page_state_offset);
		asset.pages.assign(page_ptr, page_ptr + h->page_count);
		if (h->material_count > 0 && h->material_offset > 0) {
			const auto* mat_ptr = reinterpret_cast<const bud::asset::MaterialDescriptor*>(base + h->material_offset);
			asset.materials.assign(mat_ptr, mat_ptr + h->material_count);
		}
		{
			const char* p = base + h->texture_offset;
			for (uint32_t t = 0; t < h->texture_count; ++t) {
				asset.textures.push_back(std::string(p));
				p += asset.textures.back().size() + 1;
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
		// only carries a global AABB; distance-based streaming needs per-page).
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

		auto asset_ptr = std::make_shared<BudNaniteAsset>(std::move(asset));
		{
			std::scoped_lock lock(mutex_);
			// Page raw data region is sequential: file offset = page_data_offset
			// + cumulative sizes.
			uint64_t off = asset_ptr->page_data_offset;
			for (uint32_t i = 0; i < asset_ptr->pages.size(); ++i) {
				StreamingPage sp;
				sp.asset_id = path;
				sp.page_id = i;
				sp.file_page_index = i;
				sp.file_offset = off;
				sp.capacity = asset_ptr->pages[i].size_in_bytes;
				sp.bin_path = path;
				sp.is_budnanite = true;
				sp.aabb = asset_ptr->page_aabbs[i];
				sp.has_aabb = true;
				sp.material_id = 0;
				sp.parent_page_id = asset_ptr->pages[i].dependency_page_id;

				uint32_t cs = asset_ptr->page_cluster_start[i];
				uint32_t cc = asset_ptr->page_cluster_count[i];
				if (cc > 0) {
				uint32_t current_mat = asset_ptr->clusters[cs].material_index;
					uint32_t current_lod = (bud::asset::nanite_decode_lod_error(asset_ptr->clusters[cs].lod_error) == 0.0f) ? 0 : 1;
					sp.lod_errors[0] = 0.0f;
					sp.lod_errors[1] = 0.0f;
					sp.lod_errors[2] = 0.0f;

					uint32_t run_start = cs;
					uint32_t run_indices = 0;
					uint32_t index_offset = 0;
					for (uint32_t k = 0; k < cc; ++k) {
						const auto& c = asset_ptr->clusters[cs + k];
						float err = bud::asset::nanite_decode_lod_error(c.lod_error);
						float parent_err = bud::asset::nanite_decode_lod_error(c.parent_lod_error);
						uint32_t c_lod = (err == 0.0f) ? 0 : ((err < parent_err && parent_err < FLT_MAX) ? 1 : 2); // heuristic map
						
						if (err == 0.0f) {
							sp.lod_errors[1] = std::max(sp.lod_errors[1], parent_err);
						} else {
							sp.lod_errors[2] = std::max(sp.lod_errors[2], parent_err);
						}

						if (c.material_index != current_mat || c_lod != current_lod) {
							bud::graphics::PageSubMesh psm;
							psm.index_start = index_offset;
							psm.index_count = run_indices;
							psm.material_id = current_mat;
							psm.lod_level = current_lod;
							psm.cluster_start = run_start - cs;
							psm.cluster_count = (cs + k) - run_start;
							sp.submeshes.push_back(psm);

							index_offset += run_indices;
							current_mat = c.material_index;
							current_lod = c_lod;
							run_start = cs + k;
							run_indices = 0;
						}
						run_indices += c.num_tris * 3;
					}
					bud::graphics::PageSubMesh psm;
					psm.index_start = index_offset;
					psm.index_count = run_indices;
					psm.material_id = current_mat;
					psm.lod_level = current_lod;
					psm.cluster_start = run_start - cs;
					psm.cluster_count = (cs + cc) - run_start;
					sp.submeshes.push_back(psm);
				}

				off += asset_ptr->pages[i].size_in_bytes;
				all_pages_.emplace(sp.get_unique_id(), std::move(sp));
			}
			budnanite_assets_[path] = asset_ptr;

			// Auto-register a bounding region covering all pages of this asset.
			RegionManifest region;
			region.id = path;
			region.aabb = bud::math::AABB(bud::math::vec3(-1e6f), bud::math::vec3(1e6f));
			for (uint32_t i = 0; i < asset_ptr->pages.size(); ++i)
				region.pages.push_back(path + ":page_" + std::to_string(i));
			regions_.push_back(std::move(region));

			bud::print("[Streaming] Registered .budmesh (UE5 layout): {} ({} pages, {} clusters, {} groups, {} levels)",
				path, asset_ptr->pages.size(),
				asset_ptr->clusters.size(),
				asset_ptr->groups.size(),
				asset_ptr->levels.size());
		}
	});
}

void StreamingManager::register_budmesh_async(const std::string& json_path) {
	asset_manager_->load_json_async(json_path, [this, json_path](nlohmann::json j) {
		if (j.empty() || !j.contains("magic") || j["magic"] != "BUDM") {
			bud::eprint("[Streaming] Invalid budmesh JSON: {}", json_path);
			return;
		}

		std::string data_uri = j.value("data_uri", "");
		if (data_uri.empty()) {
			bud::eprint("[Streaming] budmesh JSON missing data_uri: {}", json_path);
			return;
		}

		// Build bin path relative to json
		std::string bin_path = json_path;
		auto last_slash = bin_path.find_last_of("\\/");
		if (last_slash != std::string::npos)
			bin_path = bin_path.substr(0, last_slash + 1) + data_uri;
		else
			bin_path = data_uri;

		BudMeshAsset asset;
		asset.metadata_path = json_path;
		asset.data_uri = data_uri;

		if (j.contains("pages")) {
			uint32_t idx = 0;
			for (const auto& pj : j["pages"]) {
				StreamingPage sp;
				sp.asset_id = json_path;
				sp.page_id = pj.value("page_id", 0u);
				sp.file_page_index = idx++;
				sp.file_offset = pj.value("file_offset", 0ull);
				sp.capacity = pj.value("capacity", static_cast<uint64_t>(bud::graphics::GPUScene::PagePool::kPageSize));
				sp.bin_path = bin_path;
				sp.material_id = pj.value("material_id", 0u);
				if (pj.contains("aabb_min") && pj.contains("aabb_max")) {
					const auto& mn = pj["aabb_min"];
					const auto& mx = pj["aabb_max"];
					if (mn.is_array() && mx.is_array() && mn.size() == 3 && mx.size() == 3) {
						sp.aabb.min = bud::math::vec3(mn[0].get<float>(), mn[1].get<float>(), mn[2].get<float>());
						sp.aabb.max = bud::math::vec3(mx[0].get<float>(), mx[1].get<float>(), mx[2].get<float>());
						sp.has_aabb = true;
					}
				}
				if (pj.contains("submeshes") && pj["submeshes"].is_array()) {
					for (const auto& sm : pj["submeshes"]) {
						bud::graphics::PageSubMesh ps;
						ps.index_start = sm.value("index_start", 0u);
						ps.index_count = sm.value("index_count", 0u);
						ps.material_id = sm.value("material_id", 0u);
						ps.lod_level = sm.value("lod_level", 0u);
						ps.cluster_start = sm.value("cluster_start", 0u);
						ps.cluster_count = sm.value("cluster_count", 0u);
						if (ps.index_count > 0)
							sp.submeshes.push_back(ps);
					}
				}
				// Per-LOD index ranges, used by the shadow path to rasterize only
				// the selected LOD instead of every LOD in the page.
				if (pj.contains("lod_ranges") && pj["lod_ranges"].is_array()) {
					for (const auto& lr : pj["lod_ranges"]) {
						if (lr.is_array() && lr.size() >= 2)
							sp.lod_ranges.emplace_back(lr[0].get<uint32_t>(), lr[1].get<uint32_t>());
					}
				}
				// Nanite-like hierarchy (v3).
				sp.is_coarse = pj.value("is_coarse", false);
				sp.parent_page_id = pj.contains("parent_page_id") && !pj["parent_page_id"].is_null()
					? pj["parent_page_id"].get<uint32_t>() : bud::asset::INVALID_INDEX;
				if (pj.contains("children") && pj["children"].is_array()) {
					for (const auto& c : pj["children"])
						sp.children.push_back(c.get<uint32_t>());
				}
				asset.pages.push_back(sp);
			}
		}

		if (j.contains("textures") && j["textures"].is_array()) {
			for (const auto& t : j["textures"])
				asset.textures.push_back(t.get<std::string>());
		}

		{
				std::scoped_lock lock(mutex_);
				// CPU-driven path: register LEAF pages only. Coarse pages are
				// Nanite-style parent placeholders; leaves already contain all
				// LODs (selected by screen error), so drawing coarse pages too
				// would duplicate geometry.
				uint32_t leaf_count = 0;
				for (const auto& sp : asset.pages) {
					if (sp.is_coarse) continue;
					all_pages_.emplace(sp.get_unique_id(), sp);
					++leaf_count;
				}
				managed_assets_[json_path] = std::move(asset);

				// Auto-register a bounding region covering all pages of this asset
				RegionManifest region;
				region.id = json_path;
				region.aabb = bud::math::AABB(bud::math::vec3(-1e6f), bud::math::vec3(1e6f));
				for (const auto& sp : managed_assets_[json_path].pages) {
					if (sp.is_coarse) continue;
					region.pages.push_back(sp.get_unique_id());
				}
				regions_.push_back(std::move(region));
				bud::print("[Streaming] Registered budmesh: {} with {} leaf pages ({} total)", json_path, leaf_count, managed_assets_[json_path].pages.size());
			}
	});
}

void StreamingManager::update(const bud::math::vec3& camera_position) {
	std::vector<std::string> to_load;
	{
		std::scoped_lock lock(mutex_);
		for (const auto& [page_key, sp] : all_pages_) {
			// Per-page distance-based residency: use the distance from the
			// camera to the page's own AABB. Pages near the camera load, far
			// ones unload. Falls back to the region AABB when a page has no
			// AABB (old asset files) so nothing accidentally unloads the whole
			// scene.
			bud::math::vec3 bmin, bmax;
			if (sp.has_aabb) {
				bmin = sp.aabb.min;
				bmax = sp.aabb.max;
			} else {
				// Fallback: cover everything (keep resident while near origin).
				bmin = bud::math::vec3(-1e6f);
				bmax = bud::math::vec3(1e6f);
			}
			bud::math::vec3 closest(
				std::clamp(camera_position.x, bmin.x, bmax.x),
				std::clamp(camera_position.y, bmin.y, bmax.y),
				std::clamp(camera_position.z, bmin.z, bmax.z));
			float d = bud::math::length(camera_position - closest);

			bool resident = residency_.count(page_key) && residency_[page_key];
			if (!resident && d <= load_radius_)
				to_load.push_back(page_key);
			if (resident && d > unload_radius_) {
				if (auto it = page_gpu_slots_.find(page_key); it != page_gpu_slots_.end()) {
					gpu_scene_->update_page_table_entry(it->second, 0, 0);
					gpu_scene_->get_page_pool().free_page(it->second);
					page_gpu_slots_.erase(it);
				}
				// Notify the owner to remove the scene entity that references
				// this page's mesh; otherwise the stale entity stays in the
				// scene and later slot reuse overwrites its geometry.
				if (page_unregistered_cb_) {
					if (auto mit = page_mesh_ids_.find(page_key); mit != page_mesh_ids_.end())
						page_unregistered_cb_(mit->second);
				}
				page_mesh_ids_.erase(page_key);
					pending_loads_.erase(page_key);
					residency_.erase(page_key);
			}
		}
	}

	for (const auto& p : to_load) {
		std::scoped_lock lock(mutex_);
		if (residency_[p] || pending_loads_.count(p)) continue;
		residency_[p] = false;
		pending_loads_.insert(p);

		if (auto it = all_pages_.find(p); it != all_pages_.end()) {
			const auto& sp = it->second;
			// .budmesh pages need the resident tables to decode; keep a
			// shared_ptr so the async callback stays safe.
			std::shared_ptr<BudNaniteAsset> nanite_asset;
			if (sp.is_budnanite) {
				if (auto ait = budnanite_assets_.find(sp.asset_id); ait != budnanite_assets_.end())
					nanite_asset = ait->second;
			}
			uint64_t offset = sp.file_offset;
			asset_manager_->load_file_chunk_async(sp.bin_path, offset, sp.capacity,
				[this, p, sp, nanite_asset](std::vector<char> data) {
					if (data.empty()) {
						std::scoped_lock lock(mutex_);
						pending_loads_.erase(p);
						return;
					}

					uint32_t meshlet_count = 0;
					uint32_t index_count = 0;
					uint32_t vertex_data_offset = 0;
					uint32_t index_data_offset = 0;
					bud::math::AABB page_aabb = sp.aabb;
					const std::vector<bud::graphics::PageSubMesh>* submeshes_ptr = &sp.submeshes;
					const std::vector<std::pair<uint32_t, uint32_t>>* lod_ranges_ptr = &sp.lod_ranges;

					if (sp.is_budnanite) {
						if (data.size() < sizeof(bud::asset::NanitePageDataHeader)) {
							std::scoped_lock lock(mutex_);
							pending_loads_.erase(p);
							bud::eprint("[Streaming] .budmesh raw page chunk too small: {}", p);
							return;
						}
						const auto& ph = *reinterpret_cast<const bud::asset::NanitePageDataHeader*>(data.data());
						if (ph.magic != bud::asset::NANITE_PAGE_DATA_MAGIC) {
							std::scoped_lock lock(mutex_);
							pending_loads_.erase(p);
							bud::eprint("[Streaming] Bad .budmesh page magic 0x{:X} for: {}", ph.magic, p);
							return;
						}
						meshlet_count = ph.cluster_count;
						index_count = ph.index_count * 3u;
						vertex_data_offset = ph.vertex_stream_offset;
						index_data_offset = ph.index_stream_offset;
					}

					uint32_t slot = gpu_scene_->get_page_pool().allocate_page();
					if (slot == ~0u) {
						bud::eprint("[Streaming] Page pool exhausted for: {}", p);
						return;
					}
					uint32_t gpu_offset = gpu_scene_->get_page_pool().get_page_offset(slot);

					// Copy data to page pool via staging upload
					const char* src = data.data();
					const size_t src_size = data.size();
					if (src_size > bud::graphics::GPUScene::PagePool::kPageSize) {
						bud::eprint("[Streaming] Page {} data {} bytes exceeds page slot {}; freeing slot",
							p, src_size, bud::graphics::GPUScene::PagePool::kPageSize);
						gpu_scene_->get_page_pool().free_page(slot);
						std::scoped_lock lock(mutex_);
						pending_loads_.erase(p);
						return;
					}
					if (rhi_) {
						auto* mapped = static_cast<uint8_t*>(gpu_scene_->get_page_pool_buffer().mapped_ptr);
						if (mapped) {
							std::memcpy(mapped + gpu_offset, src, src_size);
							//bud::print("[Streaming] Copied {} bytes to page pool offset {}", src_size, gpu_offset);
						}
						else {
							bud::eprint("[Streaming] page_pool_buffer mapped_ptr is NULL!");
						}
					}

					gpu_scene_->update_page_table_entry(slot, gpu_offset);

					uint32_t mesh_id = ~0u;
					if (renderer_ && meshlet_count > 0) {
						std::vector<bud::graphics::PageSubMesh> resolved_submeshes;
						if (!submeshes_ptr->empty()) {
							resolved_submeshes.reserve(submeshes_ptr->size());
							for (const auto& sm : *submeshes_ptr) {
								bud::graphics::PageSubMesh rs;
								rs.index_start = sm.index_start;
								rs.index_count = sm.index_count;
								rs.material_id = resolve_texture_slot(sp.asset_id, sm.material_id);
								// Keep the per-(LOD, material) metadata so the renderer
								// can select one LOD level and cull the correct clusters.
								rs.lod_level = sm.lod_level;
								rs.cluster_start = sm.cluster_start;
								rs.cluster_count = sm.cluster_count;
								resolved_submeshes.push_back(rs);
							}
						}
						else {
							bud::graphics::PageSubMesh rs;
							rs.index_start = 0;
							rs.index_count = index_count;
							rs.material_id = resolve_texture_slot(sp.asset_id, sp.material_id);
							// Whole-page fallback: cover every cluster at LOD0.
							rs.cluster_start = 0;
							rs.cluster_count = meshlet_count;
							resolved_submeshes.push_back(rs);
						}
						mesh_id = renderer_->register_page_backed_mesh(slot, meshlet_count, index_count, page_aabb,
							vertex_data_offset, index_data_offset, resolved_submeshes, *lod_ranges_ptr, sp.lod_errors);
					}

					if (mesh_id != ~0u && page_registered_cb_)
						page_registered_cb_(mesh_id, page_aabb);

						{
							std::scoped_lock lock(mutex_);
							page_gpu_slots_[p] = slot;
							page_mesh_ids_[p] = mesh_id;
							residency_[p] = true;
							pending_loads_.erase(p);
						}

						//bud::print("[Streaming] Page resident: {} slot={} mesh_id={} meshlets={} idx={}",
						//	p, slot, mesh_id, meshlet_count, index_count);
					});
		}
	}
}

bool StreamingManager::is_page_resident(const std::string& page_key) const {
	std::scoped_lock lock(mutex_);
	auto it = residency_.find(page_key);
	return it != residency_.end() && it->second;
}

uint32_t StreamingManager::resolve_texture_slot(const std::string& asset_key, uint32_t material_index) {
	std::scoped_lock lock(mutex_);

	// .budmesh assets
	if (auto it = managed_assets_.find(asset_key); it != managed_assets_.end()) {
		auto& asset = it->second;
		if (material_index >= asset.textures.size())
			return 0;
		if (asset.texture_slots.empty())
			asset.texture_slots.resize(asset.textures.size(), 0);
		uint32_t& slot = asset.texture_slots[material_index];
		if (slot == 0) {
			if (!renderer_) {
				bud::eprint("[Streaming] resolve_texture_slot called but renderer is null!");
				return 0;
			}
			slot = renderer_->bind_texture_async(asset.textures[material_index]);
		}
		return slot;
	}

	// .budmesh assets
	if (auto it = budnanite_assets_.find(asset_key); it != budnanite_assets_.end()) {
		auto& asset = *it->second;
		uint32_t tex_index = bud::asset::INVALID_INDEX;
		if (material_index < asset.materials.size()) {
			tex_index = asset.materials[material_index].base_color_texture;
		}
		if (tex_index == bud::asset::INVALID_INDEX || tex_index >= asset.textures.size())
			return 0;

		if (asset.texture_slots.empty())
			asset.texture_slots.resize(asset.textures.size(), 0);
		uint32_t& slot = asset.texture_slots[tex_index];
		if (slot == 0) {
			if (!renderer_) {
				bud::eprint("[Streaming] resolve_texture_slot called but renderer is null!");
				return 0;
			}
			slot = renderer_->bind_texture_async(asset.textures[tex_index]);
		}
		return slot;
	}

	return 0;
}

} // namespace bud::streaming
