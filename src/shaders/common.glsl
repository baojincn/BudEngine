// Virtual Geometry common constants — shared across all shaders.
// Keep in sync with C++ constants in bud.asset.types.hpp and bud.graphics.gpu_scene.hpp.

const uint VG_PAGE_SIZE_BYTES = 131072u;      // 128 KB, matches GPUScene::PagePool::page_size
const uint PAGE_SIZE_BYTES = 131072u;         // Alias
const uint VG_PAGE_MAGIC = 0x50475642u;       // "BVGP" Virtual Geometry Page Data Magic
const uint VG_PAGE_HEADER_DWORDS = 16u;      // 64-byte VGPageDataHeader
const uint PAGE_HEADER_DWORDS = 16u;          // Alias
const uint VG_CLUSTER_DWORDS = 12u;           // Per-cluster layout: 7 dwords desc + 5 dwords cull data
const uint CLUSTER_DESC_DWORDS = 7u;          // Per-cluster descriptor (7 dwords)
const uint CULL_DATA_DWORDS = 5u;             // Per-cluster cull data (5 dwords)
const uint VG_MAX_VISIBLE_PAGES = 65536u;     // Max visible page buffer capacity
const uint VG_MAX_PAGE_SLOTS = 8192u;         // 13 bits (0x1FFF mask)
const uint VG_MAX_CLUSTERS_PER_PAGE = 256u;   // Max clusters per page
const uint VG_MAX_PAGE_REQUESTS = 4096u;      // Max page requests per frame
const uint VG_INVALID_INDEX = 0xFFFFFFFFu;