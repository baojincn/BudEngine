#pragma once

#include <cstdint>
#include <vector>
#include <bit>

namespace bud::asset {

    // 0x4255444D ("BUDM")
    constexpr uint32_t MESH_MAGIC = 0x4255444D;
    // bump when header layout changes
    constexpr uint32_t MESH_VERSION = 4;

#pragma pack(push, 1)

    struct SubMeshDescriptor {
        uint32_t index_start;      // Offset into the global index buffer (uint32_t indices)
        uint32_t index_count;      // Number of indices
        uint32_t meshlet_start;    // First meshlet index
        uint32_t meshlet_count;    // Number of meshlets
        uint32_t material_id;      // Material index for this submesh
        float aabb_min[3];
        float aabb_max[3];
    };

    struct BudMeshHeader {
        uint32_t magic;            // 0x4255444D
        uint32_t version;          // 3
        
        uint32_t total_vertices;
        uint32_t total_indices;
        uint32_t meshlet_count;
        uint32_t submesh_count;
        uint32_t texture_count;    // Total unique textures
        uint32_t material_count;   // Total unique materials

        float aabb_min[3];
        float aabb_max[3];

        uint64_t reserved;         // Explicit 8-byte padding to ensure 8-byte alignment for uint64_t

        uint64_t vertex_offset;
        uint64_t index_offset;
        uint64_t meshlet_offset;
        uint64_t vertex_index_offset;
        uint64_t meshlet_index_offset;
        uint64_t cull_data_offset;
        uint64_t submesh_offset;
        uint64_t material_offset;  // Offset to material table
        uint64_t texture_offset;   // Offset to the texture path list (null-terminated strings or similar)
    };

    struct MeshletDescriptor {
        uint32_t vertex_offset;    // Offset into the global vertex buffer
        uint32_t vertex_count;     // Number of unique vertices in this meshlet
        uint32_t triangle_offset;  // Offset into the index buffer
        uint32_t triangle_count;   // Number of triangles
    };

    // Per-cluster descriptor inside a page (Nanite-style, with LOD metadata).
    // 28 bytes (5 u32 + 2 f32): the GPU uses cluster_error for the single
    // threshold and parent_error for the parent-child transition (coarse LOD
    // streaming: a page may be replaced by its parent page while loading).
    struct PageClusterDesc {
        uint32_t vertex_offset;    // Page-local vertex offset
        uint32_t vertex_count;
        uint32_t triangle_offset;  // Page-local triangle offset
        uint32_t triangle_count;
        uint32_t lod_level;        // LOD level (0 = full detail)
        float cluster_error;       // Object-space error relative to LOD0
        float parent_error;        // Error of the parent (coarser) cluster this
                                   // cluster descends from; drives page fallback
    };
    static_assert(sizeof(PageClusterDesc) == 28, "PageClusterDesc must be 28 bytes");

    struct MeshletCullData {
        float bounding_sphere[4];  // x, y, z, radius
        int8_t cone_axis[3];       // Compressed normal cone axis
        int8_t cone_cutoff;        // cos(angle/2) for backface culling
    };

    // UE5-style Unified Terminology (Page-first)
    struct ClusterDescriptor {
        uint32_t vertex_offset;
        uint32_t vertex_count;
        uint32_t triangle_offset;
        uint32_t triangle_count;
        uint32_t lod_level;
        float cluster_error;
        float parent_error;
    };

    struct ClusterCullData {
        float bounding_sphere[4];
        int8_t cone_axis[3];
        int8_t cone_cutoff;
    };

    struct PageTableEntry {
        uint32_t page_id;
        uint32_t cluster_start;
        uint32_t cluster_count;
        uint64_t data_size;
        uint32_t dependency_page_id;
        uint64_t file_offset;
        uint64_t capacity;
    };

    // Vertex structure for BudEngine (Must match what is written in BudAssetTool)
    struct Vertex {
        float position[3];
        float normal[3];
        float uv[2];
        float tangent[4]; // Optional, but good to have
    };

    // Material serialization that mirrors glTF semantic choices
    enum class AlphaMode : uint8_t {
		Opaque = 0,
		Mask = 1,
		Blend = 2
    };

    struct MaterialDescriptor {
        uint32_t base_color_texture; // Index into texture table or INVALID_INDEX
        uint8_t alpha_mode;          // AlphaMode as uint8_t
        uint8_t double_sided;        // boolean (0/1)
        uint8_t padding[2];          // reserved for alignment
        float alpha_cutoff;          // used when alpha_mode == MASK
    };

#pragma pack(pop)

	// Virtual Geometry Page Binary Layout
	// Each page in the GPU Page Pool: 64-byte header + data sections
	struct PageBinaryHeader {
		static constexpr uint32_t MAGIC = 0x50414745;
		static constexpr uint32_t VERSION = 3; // v3: clusters carry parent_error; reserved = parent_page_id + coarse flag
		uint32_t magic;
		uint32_t version;
		uint32_t cluster_count;      // total clusters across all LOD levels in this page
		uint32_t vertex_count;
		uint32_t index_count;
		uint32_t parent_page_id;     // (was reserved) page id of the parent (coarser) page, INVALID_INDEX for root
		float aabb_min[3];
		float aabb_max[3];
		uint32_t vertex_data_offset;
		uint32_t index_data_offset;
		float max_error;             // coarsest-level object-space error (for screen threshold)
		uint32_t padding[1];         // bit0 = is_coarse_page (aggregated subtree geometry)
	};
	static_assert(sizeof(PageBinaryHeader) == 64, "PageBinaryHeader must stay 64 bytes");
	static_assert(sizeof(PageClusterDesc) == 28, "PageClusterDesc must be 28 bytes");
	static constexpr uint32_t PAGE_HEADER_SIZE = 64;
	static constexpr uint32_t PAGE_CLUSTER_DESC_STRIDE = sizeof(PageClusterDesc); // 28
	static constexpr uint32_t PAGE_CULL_DATA_STRIDE = sizeof(MeshletCullData); // 20 (float4 + 4x int8)
	static constexpr uint32_t PAGE_VERTEX_STRIDE = 48;

	constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;

    // Structural constants for verification
    constexpr uint32_t MESH_HEADER_SIZE = 136;
    constexpr uint32_t MESH_HEADER_VERTEX_OFFSET = 64;
    constexpr uint32_t MESH_HEADER_SUBMESH_COUNT_OFFSET = 20;
    constexpr uint32_t SUBMESH_DESCRIPTOR_SIZE = 44;

    // ====================================================================
    // BudNanite format (aligned with UE5 Nanite data layout)
    // ====================================================================
    // 与 UE5 Nanite 对齐的数据布局（文件扩展名为 .budmesh）：
    //   - cluster 是最小单元（<= 128 顶点 / 128 三角形），字段布局对齐 FCluster
    //   - 每 mesh 一棵 cluster 层次 DAG（子->父: group_index；父->子: group.children）
    //   - cluster 头常驻，page 只存 raw 位置/属性/索引（每 page 属于单个资源）
    //   - 位置按“页”量化（页数据头存 bits/offset/extent，对齐 UE5 页级 precision）；
    //     属性打包（normal/tangent R8G8B8A8_SNORM、uv 16:16、color RGBA8）；
    //     索引为页局部 u16
    // UE5 默认常量
    constexpr uint32_t NANITE_MAGIC = 0x544E4E42;            // "BNNT"
    constexpr uint32_t NANITE_VERSION = 2;
    constexpr uint32_t NANITE_PAGE_DATA_MAGIC = 0x50474142;  // "BAGP"
    constexpr uint32_t NANITE_MAX_CLUSTER_VERTICES = 128;
    constexpr uint32_t NANITE_MAX_CLUSTER_TRIANGLES = 128;
    constexpr uint32_t NANITE_PAGE_SIZE = 128 * 1024;        // 128 KB (UE5 默认)
    constexpr uint32_t NANITE_POSITION_BITS = 12;            // 每轴量化位宽 (UE5 默认)

    // LOD 误差的 u32 有序编码（对齐 UE5 FCluster::LODError 用 u32 有序编码的思路，
    // 即 UE 的 FloatToOrderedInt 方案）：单调——误差越大编码越大，decode 还原 float。
    // 编码：正数置顶位（i|0x80000000），负数取反（~i）；解码为逆操作。
    inline uint32_t nanite_encode_lod_error(float error) {
        const uint32_t bits = std::bit_cast<uint32_t>(error);
        return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
    }
    inline float nanite_decode_lod_error(uint32_t encoded) {
        const uint32_t bits = (encoded & 0x80000000u) ? (encoded ^ 0x80000000u) : ~encoded;
        return std::bit_cast<float>(bits);
    }

#pragma pack(push, 1)

    // 每资源层次级别：LOD0（最精细）.. 根（最后一层）。扩展表（UE5 无此表，
    // 层级信息由 group 顺序隐含；BudEngine 保留以便加载器快速建立每层范围）。
    struct NaniteHierarchyLevel {
        uint32_t cluster_start;      // 该层第一个 cluster 的全局索引
        uint32_t cluster_count;
        uint32_t group_start;        // 该层第一个 group 的全局索引
        uint32_t group_count;
    };

    // 对齐 UE5 FCluster（96 字节）
    struct NaniteCluster {
        uint32_t num_verts;          // <= 128
        uint32_t num_tris;           // <= 128
        uint32_t material_index;
        uint32_t position_offset;    // 页内顶点流偏移（顶点单位）
        uint32_t position_page_offset; // raw 位置数据所在页
        uint32_t index_offset;       // 页内索引流偏移（三角形单位）
        uint32_t index_page_offset;  // raw 索引数据所在页
        uint32_t group_index;        // 所属（更粗）group；root 为 INVALID_INDEX
        uint32_t lod_error;          // u32 有序编码：相对 LOD0 的对象空间误差
        uint32_t parent_lod_error;   // 父 cluster 的误差（页回退用），同编码
        // 位置包围盒（对象空间；页级量化时用于校验/回读）
        float position_bounds_center[3];
        float position_bounds_extent[3];
        // LOD 包围球（剔除）
        float lod_bounds_center[3];
        float lod_bounds_radius;
        // 法锥剔除
        float cone_axis[3];
        float cone_cutoff;
    };
    static_assert(sizeof(NaniteCluster) == 96, "NaniteCluster must be 96 bytes");

    // 对齐 UE5 FClusterGroup（36 字节）
    struct NaniteClusterGroup {
        uint32_t page_index_start;   // 组内 cluster 覆盖的首页
        uint32_t page_index_num;
        uint32_t children_start;     // 子 group 全局索引（父->子 DAG）
        uint32_t children_num;       // 1 或 2（二分 DAG）；根组为 0
        float lod_bounds_center[3];
        float lod_bounds_radius;
        float lod_error;
    };
    static_assert(sizeof(NaniteClusterGroup) == 36, "NaniteClusterGroup must be 36 bytes");

    // 对齐 UE5 FPageStreamingState（前 28 字节）+ BudEngine 扩展（尾部 8 字节）
    struct NanitePageStreamingState {
        uint32_t raw_vertex_offset;  // 页数据区内顶点流偏移（字节）
        uint32_t raw_vertex_count;
        uint32_t raw_index_offset;   // 页数据区内索引流偏移（字节）
        uint32_t raw_index_count;    // 页内三角形数（页局部 u16 索引总数 = raw_index_count*3）
        uint32_t imposter_offset;    // 预留（imposter）
        uint32_t imposter_count;     // 预留（imposter）
        uint32_t flags;              // bit0: root 常驻
        // --- BudEngine 扩展 ---
        uint32_t dependency_page_id; // 父页；INVALID_INDEX = 无依赖（根常驻）
        uint32_t size_in_bytes;      // 页数据总字节（含页头）
    };
    static_assert(sizeof(NanitePageStreamingState) == 36, "NanitePageStreamingState must be 36 bytes");

    // 对齐 UE5 FPageDependency（12 字节）
    struct NanitePageDependency {
        uint32_t page_id;            // 被依赖的页（父页）
        uint32_t start_group_index;  // 依赖的组范围
        uint32_t num_groups;
    };
    static_assert(sizeof(NanitePageDependency) == 12, "NanitePageDependency must be 12 bytes");

    // .budnanite 文件头
    struct NaniteHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t flags;
        uint32_t cluster_count;
        uint32_t group_count;
        uint32_t hierarchy_level_count;
        uint32_t page_count;
        uint32_t dependency_count;
        uint32_t material_count;
        uint32_t texture_count;
        uint64_t cluster_offset;
        uint64_t group_offset;
        uint64_t hierarchy_offset;
        uint64_t page_state_offset;
        uint64_t dependency_offset;
        uint64_t material_offset;
        uint64_t texture_offset;
        uint64_t page_data_offset;
        float aabb_min[3];
        float aabb_max[3];
        uint64_t reserved[2];
    };
    static_assert(sizeof(NaniteHeader) == 144, "NaniteHeader must be 144 bytes");

    // 页数据头（每页 raw 数据起始处，64 字节）。
    // 位置为页级量化：页内所有顶点统一按 position_bits 位量化到
    // [position_offset, position_offset + position_extent] 的页 AABB 内。
    struct NanitePageDataHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t cluster_count;
        uint32_t vertex_count;
        uint32_t index_count;        // 页内三角形数；页局部 u16 索引总数为 index_count*3
        uint32_t vertex_stream_offset; // 相对页数据起始（字节）
        uint32_t index_stream_offset;  // 相对页数据起始（字节）
        uint32_t total_size;
        uint32_t flags;
        uint32_t position_bits;      // 每轴量化位宽（页级，默认 12）
        float position_offset[3];    // 页 AABB min（量化原点）
        float position_extent[3];    // 页 AABB max-min（量化范围）
    };
    static_assert(sizeof(NanitePageDataHeader) == 64, "NanitePageDataHeader must be 64 bytes");

    // 页内打包属性顶点（对齐 UE5 FPackedNormal / FPackedRGBA16N 思路）
    struct NanitePackedVertex {
        uint8_t normal[4];           // R8G8B8A8_SNORM
        uint8_t tangent[4];          // R8G8B8A8_SNORM
        uint16_t uv[2];              // 16:16
        uint32_t color;              // RGBA8（预留）
    };
    static_assert(sizeof(NanitePackedVertex) == 16, "NanitePackedVertex must be 16 bytes");

#pragma pack(pop)

} // namespace bud::asset
