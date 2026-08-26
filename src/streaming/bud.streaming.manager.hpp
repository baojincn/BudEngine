#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <memory>

#include "src/io/bud.io.hpp"
#include "src/core/bud.math.hpp"
#include "src/graphics/bud.graphics.types.hpp"
#include "src/graphics/bud.graphics.gpu_scene.hpp"

namespace bud::graphics {
	class Renderer;
	class RHI;
}

namespace bud::streaming {

struct StreamingPage {
	std::string asset_id;
	uint32_t page_id;
	uint64_t file_offset;
	uint64_t capacity;
	std::string bin_path;
	uint32_t virtual_page_index = 0;
	// True when the page raw data comes from a Virtual Geometry asset
	// (quantized positions + packed attributes + u16 indices).
	bool is_virtual_geometry = false;
	// World-space AABB of the page's geometry, exported by BudAssetTool and
	// used for distance-based eviction.
	bud::math::AABB aabb;
	bud::math::AABB global_aabb;
	bool has_aabb = false;
	// Optional dependency: the page that must be loaded before this one.
	// INVALID_INDEX means no dependency (root page).
	uint32_t dependency_page_id = bud::asset::INVALID_INDEX;
	std::string get_unique_id() const { return asset_id + ":page_" + std::to_string(page_id); }
};

// Resident tables parsed from a Virtual Geometry asset. Clusters/groups/levels are
// kept on CPU permanently; pages stream their raw data on demand.
struct VirtualGeometryAsset {
	std::string path;
	uint64_t page_data_offset = 0;
	uint32_t root_group_index = 0;
	std::vector<bud::asset::VGCluster> clusters;
	std::vector<bud::asset::VGClusterGroup> groups;
	std::vector<bud::asset::VGPageStreamingState> pages;
	std::vector<bud::asset::MaterialDescriptor> materials;
	std::vector<std::string> textures;
	// Per-page world-space AABB, computed from the cluster position bounds.
	std::vector<bud::math::AABB> page_aabbs;
	// Per-page cluster range in the cluster table.
	std::vector<uint32_t> page_cluster_start;
	std::vector<uint32_t> page_cluster_count;
};

class StreamingManager {
public:
	using AssetRegisteredCallback = std::function<void(uint32_t mesh_id, const bud::math::AABB& aabb, uint32_t root_group_index, uint32_t base_virtual_page)>;

	StreamingManager(bud::io::AssetManager* asset_manager,
		bud::graphics::GPUScene* gpu_scene,
		bud::graphics::Renderer* renderer,
		bud::graphics::RHI* rhi);

	void set_asset_registered_callback(AssetRegisteredCallback cb) { asset_registered_callback = std::move(cb); }

	// Processes page faults emitted by the GPU hierarchy traversal pass
	// (virtual page indices read back from the PageRequestBuffer) and starts
	// async loads for pages that are not resident and not already pending.
	void process_gpu_page_requests(const uint32_t* virtual_page_indices, uint32_t count);

	// Registers a Virtual Geometry asset for streaming: parses resident tables
	// (clusters/groups/pages), allocates virtual pages and uploads the group hierarchy.
	// Page raw data is then streamed on demand from .budbulk.
	void register_virtual_geometry_async(const std::string& path);
	// Unregisters a previously registered Virtual Geometry asset and frees its
	// virtual page slots for reuse.
	void unregister_virtual_geometry(const std::string& path);
	// Evicts resident pages that are farther than unload_radius_ from the
	// camera. Loading itself is demand-driven by GPU page faults.
	void update(const bud::math::vec3& camera_position);
	bool is_page_resident(const std::string& page_key) const;

private:
	bud::io::AssetManager* asset_manager = nullptr;
	bud::graphics::GPUScene* gpu_scene = nullptr;
	bud::graphics::Renderer* renderer = nullptr;
	bud::graphics::RHI* rhi = nullptr;

	// Resident Virtual Geometry tables, kept alive for the lifetime of the asset.
	std::unordered_map<std::string, std::shared_ptr<VirtualGeometryAsset>> virtual_geometry_assets;
	std::unordered_map<std::string, StreamingPage> all_pages;

	mutable std::mutex mutex_sm;
	std::unordered_map<std::string, bool> residency_sm;
	std::unordered_set<std::string> pending_loads;
	std::unordered_map<std::string, uint32_t> page_gpu_slots;

	// virtual page index -> page key ("asset_path:page_N").
	// Lock ordering: always take mutex_sm before vpk_mutex.
	std::vector<std::string> virtual_page_keys;
	std::mutex vpk_mutex;
	// Free slots in virtual_page_keys that can be reused.
	std::vector<uint32_t> free_virtual_page_slots;
	std::mutex fvps_mutex;

	AssetRegisteredCallback asset_registered_callback;

	// Eviction threshold in world units (≈1 unit = 1 cm). Sponza's exported
	// scene spans ~3700 units (BudAssetTool keeps the obj scale). 2500 keeps
	// the whole scene resident while the camera is inside it.
	float unload_radius_ = 2500.0f;

	// Retry queue for page requests that failed due to pool exhaustion.
	// These will be retried in the next frame's process_gpu_page_requests().
	// Max size limited to prevent unbounded memory growth.
	std::vector<std::string> retry_queue;
	std::mutex retry_mutex;
	static constexpr size_t max_retry_queue_size = 4096;

	// Maximum number of pages to evict per frame when pool is exhausted.
	static constexpr uint32_t max_evict_per_frame = 16;

	// LRU page access tracking: records the last frame each page was accessed.
	// Used by evict_furthest_pages to prioritize eviction of least-recently-used pages.
	struct PageAccessRecord {
		uint64_t last_access_frame = 0;
		float distance_to_camera = FLT_MAX;
	};
	mutable std::mutex access_mutex;
	std::unordered_map<std::string, PageAccessRecord> page_access_records;
	uint64_t current_frame_number = 0;

	// Evict the furthest resident pages to free pool slots.
	void evict_furthest_pages(uint32_t count, const bud::math::vec3& camera_position);

	// Process page requests from a list of page keys (used by retry queue).
	void process_gpu_page_requests_from_keys(const std::vector<std::string>& page_keys);
};

} // namespace bud::streaming
