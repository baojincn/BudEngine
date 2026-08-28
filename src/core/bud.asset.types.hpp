#pragma once

#include <cstdint>
#include <vector>
#include <bit>

namespace bud::asset {

	constexpr uint64_t generate_magic_number(
		char a, char b, char c, char d,
		char e, char f, char g, char h) {
		return
			((uint64_t)a << 56) |
			((uint64_t)b << 48) |
			((uint64_t)c << 40) |
			((uint64_t)d << 32) |
			((uint64_t)e << 24) |
			((uint64_t)f << 16) |
			((uint64_t)g << 8) |
			((uint64_t)h);
	}

	constexpr uint64_t BUD_ASSET_MAGIC = generate_magic_number('B', 'U', 'D', 'A', 'S', 'S', 'E', 'T');
	constexpr uint32_t BUD_ASSET_VERSION = 1;
	constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;

	enum class AssetType : uint32_t {
		Mesh = 0,
		Texture = 1,
		Material = 2,
		Animation = 3,
		Physics = 4,
		Scene = 5
	};

	enum class AssetChunkType : uint32_t {
		AssetManifest = 1,
		VirtualGeometry = 2,
		Material = 3,
		Texture = 4,
		Collision = 5,
		RawMesh = 6,
		RawTexture = 7
	};

	enum class AssetChunkFlags : uint32_t {
		None = 0,
		BulkData = 1 << 0,
		Compressed = 1 << 1
	};

	#pragma pack(push, 1)

	// .budasset Root Container Header
	struct BudAssetHeader {
		uint64_t magic;
		uint32_t version;
		uint32_t asset_type;
		uint64_t asset_id;
		uint32_t flags;
		uint32_t chunk_count;
		uint64_t chunk_table_offset;
		uint64_t bulk_data_size;
	};
	static_assert(sizeof(BudAssetHeader) == 48, "BudAssetHeader must be 48 bytes");

	// Chunk Table Entry
	struct AssetChunkEntry {
		uint32_t chunk_type;
		uint32_t flags;
		uint64_t offset;
		uint64_t size;
	};
	static_assert(sizeof(AssetChunkEntry) == 24, "AssetChunkEntry must be 24 bytes");

	// Basic Vertex structure for input & intermediate geometry
	struct Vertex {
		float position[3];
		float normal[3];
		float uv[2];
		float tangent[4];
	};

	enum class AlphaMode : uint8_t {
		Opaque = 0,
		Mask = 1,
		Blend = 2
	};

	struct MaterialDescriptor {
		uint32_t base_color_texture;
		uint8_t alpha_mode;
		uint8_t double_sided;
		uint8_t padding[2];
		float alpha_cutoff;
		uint64_t material_asset_id = 0; // Reference to external Material .budasset
	};

	// Generic Asset Reference Entry
	struct AssetReference {
		uint64_t asset_id;
		char relative_path[256];
	};

	// Texture Asset Types
	constexpr uint32_t TEXTURE_MAGIC = 0x58455442; // "BTEX"
	constexpr uint32_t TEXTURE_VERSION = 1;

	enum class TextureFormat : uint32_t {
		RGBA8_UNORM = 0,
		BC7_UNORM = 1,
		BC5_UNORM = 2,
		RGBA16_FLOAT = 3
	};

	struct TextureHeader {
		uint32_t magic;
		uint32_t version;
		uint32_t width;
		uint32_t height;
		uint32_t depth;
		uint32_t mip_levels;
		uint32_t format;
		uint32_t flags;
		uint64_t total_data_size;
	};

	struct TextureMipEntry {
		uint32_t mip_level;
		uint32_t width;
		uint32_t height;
		uint32_t size_in_bytes;
		uint64_t offset_in_payload;
	};

	// Material Asset Types
	constexpr uint32_t MATERIAL_MAGIC = 0x54414D42; // "BMAT"
	constexpr uint32_t MATERIAL_VERSION = 1;

	struct RuntimeMaterialHeader {
		uint32_t magic;
		uint32_t version;
		uint8_t alpha_mode;
		uint8_t double_sided;
		uint8_t shading_model;
		uint8_t padding;
		float base_color_factor[4];
		float metallic_factor;
		float roughness_factor;
		float alpha_cutoff;
		uint32_t texture_count;
		uint64_t texture_asset_ids[8];
	};

	constexpr uint32_t BUD_ASSET_FLAG_NONE = 0;
	constexpr uint32_t BUD_ASSET_FLAG_HAS_BULK_DATA = 1 << 0;

	// ====================================================================
	// Virtual Geometry 1:1 UE5-aligned Data Layout
	// ====================================================================
	constexpr uint32_t VG_MAGIC = 0x54475642;            // "BVGT" (Bud Virtual Geometry Tree)
	constexpr uint32_t VG_VERSION = 1;
	constexpr uint32_t VG_PAGE_DATA_MAGIC = 0x50475642;  // "BVGP" (Bud Virtual Geometry Page)
	constexpr uint32_t VG_MAX_CLUSTER_VERTICES = 128;
	constexpr uint32_t VG_MAX_CLUSTER_TRIANGLES = 128;
	constexpr uint32_t VG_PAGE_SIZE = 128 * 1024;        // 128 KB standard streaming page
	constexpr uint32_t VG_POSITION_BITS = 12;            // Per-axis quantization bitwidth
	constexpr uint32_t VG_PAGE_FLAG_INLINE = 0;
	constexpr uint32_t VG_PAGE_FLAG_STREAMABLE = 1 << 0;

	// LOD error ordered uint32 encoding
	inline uint32_t vg_encode_lod_error(float error) {
		const uint32_t bits = std::bit_cast<uint32_t>(error);
		if (bits & 0x80000000u)
			return ~bits;
		else
			return bits | 0x80000000u;
	}

	inline float vg_decode_lod_error(uint32_t encoded) {
		const uint32_t bits = (encoded & 0x80000000u) ? (encoded ^ 0x80000000u) : ~encoded;
		return std::bit_cast<float>(bits);
	}

	// 1:1 Aligned with UE5 FCluster (96 bytes)
	struct VGCluster {
		uint32_t num_verts;
		uint32_t num_tris;
		uint32_t material_index;
		uint32_t position_offset;      // Vertex stream offset in page
		uint32_t position_page_offset; // Page index containing raw position stream
		uint32_t index_offset;         // Index stream offset in page
		uint32_t index_page_offset;    // Page index containing raw index stream
		uint32_t group_index;          // Parent cluster group index; root group is INVALID_INDEX
		uint32_t lod_error;            // Encoded u32 object-space error relative to LOD0
		uint32_t parent_lod_error;     // Encoded u32 parent cluster error (for fallback)
		
		float position_bounds_center[3];
		float position_bounds_extent[3];
		
		float lod_bounds_center[3];
		float lod_bounds_radius;
		
		float cone_axis[3];
		float cone_cutoff;
	};
	static_assert(sizeof(VGCluster) == 96, "VGCluster must be 96 bytes");

	// 1:1 Aligned with UE5 FClusterGroup (36 bytes)
	struct VGClusterGroup {
		uint32_t page_index_start;
		uint32_t page_index_num;
		uint32_t children_start;
		uint32_t children_num;
		float lod_bounds_center[3];
		float lod_bounds_radius;
		float lod_error;
	};
	static_assert(sizeof(VGClusterGroup) == 36, "VGClusterGroup must be 36 bytes");

	// 1:1 Aligned with UE5 FPageStreamingState (36 bytes)
	struct VGPageStreamingState {
		uint32_t raw_vertex_offset;
		uint32_t raw_vertex_count;
		uint32_t raw_index_offset;
		uint32_t raw_index_count;
		uint32_t imposter_offset;
		uint32_t imposter_count;
		uint32_t flags;
		uint32_t dependency_page_id; // Parent page dependency; INVALID_INDEX = root resident
		uint32_t size_in_bytes;
	};
	static_assert(sizeof(VGPageStreamingState) == 36, "VGPageStreamingState must be 36 bytes");

	// 1:1 Aligned with UE5 FPageDependency (12 bytes)
	struct VGPageDependency {
		uint32_t page_id;
		uint32_t start_group_index;
		uint32_t num_groups;
	};
	static_assert(sizeof(VGPageDependency) == 12, "VGPageDependency must be 12 bytes");

	// Virtual Geometry Chunk Header (128 bytes)
	struct VGHeader {
		uint32_t magic;
		uint32_t version;
		uint32_t flags;
		uint32_t cluster_count;
		uint32_t group_count;
		uint32_t page_count;
		uint32_t dependency_count;
		uint32_t material_count;
		uint32_t texture_count;
		uint32_t root_group_index;
		uint64_t cluster_offset;
		uint64_t group_offset;
		uint64_t page_state_offset;
		uint64_t dependency_offset;
		uint64_t material_offset;
		uint64_t texture_offset;
		uint64_t page_data_offset;
		float aabb_min[3];
		float aabb_max[3];
		uint64_t reserved[1];
	};
	static_assert(sizeof(VGHeader) == 128, "VGHeader must be 128 bytes");

	inline constexpr uint32_t VG_PAGE_MAGIC = 0x50414745; // 'PAGE'
	inline constexpr uint32_t VG_PAGE_VERSION = 1;
	inline constexpr uint32_t VG_PAGE_SLOT_MASK = 0x1FFF; // 13-bit slot index (up to 8192 slots)
	inline constexpr uint32_t VG_MAX_VISIBLE_PAGES = 4096; // GPU readback clamp
	inline constexpr uint32_t VG_DEFAULT_ESTIMATED_PAGE_TRIANGLES = 1280; // Fallback triangle estimate per page

	// 1:1 Aligned with UE5 Page Data Header (64 bytes)
	struct VGPageDataHeader {
		uint32_t magic;
		uint32_t version;
		uint32_t cluster_count;
		uint32_t vertex_count;
		uint32_t index_count;
		uint32_t vertex_stream_offset;
		uint32_t index_stream_offset;
		uint32_t total_size;
		uint32_t flags;
		uint32_t position_bits;
		float position_offset[3];
		float position_extent[3];
	};
	static_assert(sizeof(VGPageDataHeader) == 64, "VGPageDataHeader must be 64 bytes");

	// 1:1 Aligned with UE5 FPackedNormal / FPackedRGBA16N (16 bytes)
	struct VGPackedVertex {
		uint8_t normal[4];
		uint8_t tangent[4];
		uint16_t uv[2];
		uint32_t color;
	};
	static_assert(sizeof(VGPackedVertex) == 16, "VGPackedVertex must be 16 bytes");

	#pragma pack(pop)

} // namespace bud::asset
