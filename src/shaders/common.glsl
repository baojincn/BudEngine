// Virtual Geometry common constants — shared across all shaders.
// Keep in sync with C++ constants in bud.asset.types.hpp and bud.graphics.gpu_scene.hpp.

const uint PAGE_SIZE_BYTES = 131072;       // 128 KB, matches GPUScene::PagePool::page_size
const uint PAGE_HEADER_DWORDS = 16u;       // 64-byte VGPageDataHeader
const uint CLUSTER_DESC_DWORDS = 7u;       // Per-cluster descriptor (7 dwords)
const uint CULL_DATA_DWORDS = 5u;          // Per-cluster cull data (5 dwords)