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
			for (const auto& pj : j["pages"]) {
				StreamingPage sp;
				sp.asset_id = json_path;
				sp.page_id = pj.value("page_id", 0u);
				sp.file_offset = pj.value("file_offset", 0ull);
				sp.capacity = pj.value("capacity", 131072ull);
				sp.bin_path = bin_path;
				asset.pages.push_back(sp);
			}
		}

		{
			std::scoped_lock lock(mutex_);
			for (const auto& sp : asset.pages)
				all_pages_.emplace(sp.get_unique_id(), sp);
			managed_assets_[json_path] = std::move(asset);
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
					residency_.erase(p);
				}
			}
		}
	}

	for (const auto& p : to_load) {
		std::scoped_lock lock(mutex_);
		if (residency_[p]) continue;
		residency_[p] = false;

		if (auto it = all_pages_.find(p); it != all_pages_.end()) {
			const auto& sp = it->second;
			asset_manager_->load_file_chunk_async(sp.bin_path, sp.file_offset, sp.capacity,
				[this, p, sp](std::vector<char> data) {
					if (data.empty()) return;

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
						auto* allocator = rhi_->get_allocator();
						if (allocator) {
							auto* mapped = static_cast<uint8_t*>(gpu_scene_->get_page_pool_buffer().mapped_ptr);
							if (mapped) {
								std::memcpy(mapped + gpu_offset, data.data(), data.size());
							}
						}
					}

					gpu_scene_->update_page_table_entry(slot, gpu_offset);

					uint32_t mesh_id = ~0u;
					if (renderer_ && meshlet_count > 0) {
						mesh_id = renderer_->register_page_backed_mesh(slot, meshlet_count, index_count, page_aabb,
							vertex_data_offset, index_data_offset);
					}

					if (mesh_id != ~0u && page_registered_cb_)
						page_registered_cb_(mesh_id, page_aabb);

					{
						std::scoped_lock lock(mutex_);
						page_gpu_slots_[p] = slot;
						page_mesh_ids_[p] = mesh_id;
						residency_[p] = true;
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

} // namespace bud::streaming
