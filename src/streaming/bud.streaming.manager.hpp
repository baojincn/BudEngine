#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"

namespace bud::graphics { class Renderer; class RHI; }

namespace bud::streaming {

struct StreamingPage {
	std::string asset_id;
	uint32_t page_id;
	uint32_t file_page_index;
	uint64_t file_offset;
	uint64_t capacity;
	std::string bin_path;
	uint32_t material_id = 0; // base color texture index (into BudMeshAsset::textures)
	std::vector<bud::graphics::PageSubMesh> submeshes; // per-material runs (material_id = texture index)
	std::string get_unique_id() const { return asset_id + ":page_" + std::to_string(page_id); }
};

struct BudMeshAsset {
	std::string metadata_path;
	std::string data_uri;
	std::vector<StreamingPage> pages;
	std::vector<std::string> textures;
	std::vector<uint32_t> texture_slots; // parallel to textures: resolved bindless slots (0 = unbound)
};

struct RegionManifest {
	std::string id;
	bud::math::AABB aabb;
	std::vector<std::string> pages;
	int priority = 0;
};

class StreamingManager {
public:
	using PageRegisteredCallback = std::function<void(uint32_t mesh_id, const bud::math::AABB& aabb)>;

	StreamingManager(bud::io::AssetManager* asset_manager,
		bud::graphics::GPUScene* gpu_scene,
		bud::graphics::Renderer* renderer,
		bud::graphics::RHI* rhi);

	void set_page_registered_callback(PageRegisteredCallback cb) { page_registered_cb_ = std::move(cb); }

	void register_budmesh_async(const std::string& json_path);
	void register_region(const RegionManifest& region);
	void update(const bud::math::vec3& camera_position);
	bool is_page_resident(const std::string& page_key) const;

private:
	// Resolves a base color texture index to a bindless slot, reserving/loading
	// the texture on first use. Returns 0 if unavailable (fallback/grey).
	uint32_t resolve_texture_slot(const std::string& asset_key, uint32_t tex_index);

private:
	bud::io::AssetManager* asset_manager_ = nullptr;
	bud::graphics::GPUScene* gpu_scene_ = nullptr;
	bud::graphics::Renderer* renderer_ = nullptr;
	bud::graphics::RHI* rhi_ = nullptr;

	std::vector<RegionManifest> regions_;
	std::unordered_map<std::string, BudMeshAsset> managed_assets_;
	std::unordered_map<std::string, StreamingPage> all_pages_;

	mutable std::mutex mutex_;
	std::unordered_map<std::string, bool> residency_;
	std::unordered_set<std::string> pending_loads_;
	std::unordered_map<std::string, uint32_t> page_gpu_slots_;
	std::unordered_map<std::string, uint32_t> page_mesh_ids_;

	PageRegisteredCallback page_registered_cb_;

	float load_radius_ = 150.0f;
	float unload_radius_ = 200.0f;
};

} // namespace bud::streaming
