#include "src/streaming/bud.streaming.manager.hpp"
#include "src/core/bud.logger.hpp"
#include "src/core/bud.asset.types.hpp"
#include "src/graphics/bud.graphics.renderer.hpp"
#include "src/graphics/bud.graphics.rhi.hpp"
#include <nlohmann/json.hpp>

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
				if (pj.contains("submeshes") && pj["submeshes"].is_array()) {
					for (const auto& sm : pj["submeshes"]) {
						bud::graphics::PageSubMesh ps;
						ps.index_start = sm.value("index_start", 0u);
						ps.index_count = sm.value("index_count", 0u);
						ps.material_id = sm.value("material_id", 0u);
						if (ps.index_count > 0)
							sp.submeshes.push_back(ps);
					}
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
				for (const auto& sp : asset.pages)
					all_pages_.emplace(sp.get_unique_id(), sp);
				managed_assets_[json_path] = std::move(asset);

				// Auto-register a bounding region covering all pages of this asset
				RegionManifest region;
				region.id = json_path;
				region.aabb = bud::math::AABB(bud::math::vec3(-1e6f), bud::math::vec3(1e6f));
				for (const auto& sp : managed_assets_[json_path].pages)
					region.pages.push_back(sp.get_unique_id());
				regions_.push_back(std::move(region));
			}

			bud::print("[Streaming] Registered budmesh: {} with {} pages", json_path, managed_assets_[json_path].pages.size());
	});
}

void StreamingManager::update(const bud::math::vec3& camera_position) {
	std::vector<std::string> to_load;
	{
		std::scoped_lock lock(mutex_);
		for (const auto& r : regions_) {
			float d = std::sqrt(bud::math::distance2(camera_position, r.aabb.center()));
			for (const auto& p : r.pages) {
				bool resident = residency_.count(p) && residency_[p];
				if (!resident && d <= load_radius_)
					to_load.push_back(p);
				if (resident && d > unload_radius_) {
					if (auto it = page_gpu_slots_.find(p); it != page_gpu_slots_.end()) {
						gpu_scene_->get_page_pool().free_page(it->second);
						page_gpu_slots_.erase(it);
					}
					page_mesh_ids_.erase(p);
						pending_loads_.erase(p);
						residency_.erase(p);
				}
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
				uint64_t offset = sp.file_offset;
			asset_manager_->load_file_chunk_async(sp.bin_path, offset, sp.capacity,
				[this, p, sp](std::vector<char> data) {
					if (data.empty()) {
						std::scoped_lock lock(mutex_);
						pending_loads_.erase(p);
						return;
					}

					uint32_t meshlet_count = 0;
					uint32_t index_count = 0;
					uint32_t vertex_data_offset = 0;
					uint32_t index_data_offset = 0;
					bud::math::AABB page_aabb;
					if (data.size() >= sizeof(bud::asset::PageBinaryHeader)) {
						auto* hdr = reinterpret_cast<const bud::asset::PageBinaryHeader*>(data.data());
						if (hdr->magic == bud::asset::PageBinaryHeader::MAGIC) {
								meshlet_count = hdr->meshlet_count;
								index_count = hdr->index_count;
								vertex_data_offset = hdr->vertex_data_offset;
								index_data_offset = hdr->index_data_offset;
								page_aabb.min = bud::math::vec3(hdr->aabb_min[0], hdr->aabb_min[1], hdr->aabb_min[2]);
								page_aabb.max = bud::math::vec3(hdr->aabb_max[0], hdr->aabb_max[1], hdr->aabb_max[2]);
							} else {
								bud::eprint("[Streaming] Bad page header magic=0x{:X} size={}", hdr->magic, data.size());
							}
					}

					uint32_t slot = gpu_scene_->get_page_pool().allocate_page();
					if (slot == ~0u) {
						bud::eprint("[Streaming] Page pool exhausted for: {}", p);
						return;
					}
					uint32_t gpu_offset = gpu_scene_->get_page_pool().get_page_offset(slot);

					// Copy data to page pool via staging upload
					if (rhi_) {
							auto* mapped = static_cast<uint8_t*>(gpu_scene_->get_page_pool_buffer().mapped_ptr);
							if (mapped) {
								std::memcpy(mapped + gpu_offset, data.data(), data.size());
								bud::print("[Streaming] Copied {} bytes to page pool offset {}", data.size(), gpu_offset);
							} else {
								bud::eprint("[Streaming] page_pool_buffer mapped_ptr is NULL!");
							}
						}

					gpu_scene_->update_page_table_entry(slot, gpu_offset);

					uint32_t mesh_id = ~0u;
					if (renderer_ && meshlet_count > 0) {
						std::vector<bud::graphics::PageSubMesh> resolved_submeshes;
						if (!sp.submeshes.empty()) {
							resolved_submeshes.reserve(sp.submeshes.size());
							for (const auto& sm : sp.submeshes) {
								bud::graphics::PageSubMesh rs;
								rs.index_start = sm.index_start;
								rs.index_count = sm.index_count;
								rs.material_id = resolve_texture_slot(sp.asset_id, sm.material_id);
								resolved_submeshes.push_back(rs);
							}
						}
						else {
							bud::graphics::PageSubMesh rs;
							rs.index_start = 0;
							rs.index_count = index_count;
							rs.material_id = resolve_texture_slot(sp.asset_id, sp.material_id);
							resolved_submeshes.push_back(rs);
						}
						mesh_id = renderer_->register_page_backed_mesh(slot, meshlet_count, index_count, page_aabb,
							vertex_data_offset, index_data_offset, resolved_submeshes);
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

						bud::print("[Streaming] Page resident: {} slot={} mesh_id={} meshlets={}",
							p, slot, mesh_id, meshlet_count);
					});
		}
	}
}

bool StreamingManager::is_page_resident(const std::string& page_key) const {
	std::scoped_lock lock(mutex_);
	auto it = residency_.find(page_key);
	return it != residency_.end() && it->second;
}

uint32_t StreamingManager::resolve_texture_slot(const std::string& asset_key, uint32_t tex_index) {
	std::scoped_lock lock(mutex_);

	auto it = managed_assets_.find(asset_key);
	if (it == managed_assets_.end() || tex_index >= it->second.textures.size())
		return 0;

	auto& asset = it->second;
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

} // namespace bud::streaming
