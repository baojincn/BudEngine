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
	// World-space AABB of the page's geometry, exported by BudAssetTool and
	// used for distance-based on-demand streaming.
	bud::math::AABB aabb;
	bool has_aabb = false;
	std::vector<bud::graphics::PageSubMesh> submeshes; // per-(LOD, material) runs
	// Per-LOD index ranges [start, count] inside the page's index data
	// (from the JSON lod_ranges). The page index stream is ordered LOD0->LOD2.
	std::vector<std::pair<uint32_t, uint32_t>> lod_ranges;
	// Nanite-like hierarchy (v3): coarse pages aggregate a subtree's LOD2
	// clusters. In the CPU-driven path they are skipped (leaves already contain
	// all LODs and select LOD by screen error); kept for future GPU-driven
	// hierarchy traversal.
	bool is_coarse = false;
	uint32_t parent_page_id = bud::asset::INVALID_INDEX;
	std::vector<uint32_t> children;
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
	using PageUnregisteredCallback = std::function<void(uint32_t mesh_id)>;

	StreamingManager(bud::io::AssetManager* asset_manager,
		bud::graphics::GPUScene* gpu_scene,
		bud::graphics::Renderer* renderer,
		bud::graphics::RHI* rhi);

	void set_page_registered_callback(PageRegisteredCallback cb) { page_registered_cb_ = std::move(cb); }
	// Called when a page is unloaded so the owner can remove the scene entity
	// referencing its mesh. Without this, unloaded meshes stay in the scene and
	// later slot reuse overwrites their geometry (broken vertices at origin).
	void set_page_unregistered_callback(PageUnregisteredCallback cb) { page_unregistered_cb_ = std::move(cb); }

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
	PageUnregisteredCallback page_unregistered_cb_;

	// Distance thresholds in world units (≈1 unit = 1 m). Sponza's exported
	// scene spans ~3700 units (BudAssetTool keeps the obj scale). 2000/2500
	// keeps the whole scene resident while the camera is inside it, with
	// hysteresis to avoid load/unload thrash at the boundary.
	// Distance thresholds in world units (≈1 unit = 1 m). Sponza's exported
	// scene spans ~3700 units (BudAssetTool keeps the obj scale). 2000/2500
	// keeps the whole scene resident while the camera is inside it, with
	// hysteresis to avoid load/unload thrash at the boundary.
	float load_radius_ = 2000.0f;
	float unload_radius_ = 2500.0f;
};

} // namespace bud::streaming
