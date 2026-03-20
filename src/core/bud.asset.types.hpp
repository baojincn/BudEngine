#pragma once

#include <cstdint>
#include <vector>

namespace bud::asset {

    // 0x4255444D ("BUDM")
    constexpr uint32_t MESH_MAGIC = 0x4255444D;
    constexpr uint32_t MESH_VERSION = 1;

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
        uint64_t texture_offset;   // Offset to the texture path list (null-terminated strings or similar)
    };

    struct MeshletDescriptor {
        uint32_t vertex_offset;    // Offset into the global vertex buffer
        uint32_t vertex_count;     // Number of unique vertices in this meshlet
        uint32_t triangle_offset;  // Offset into the index buffer
        uint32_t triangle_count;   // Number of triangles
    };

    struct MeshletCullData {
        float bounding_sphere[4];  // x, y, z, radius
        int8_t cone_axis[3];       // Compressed normal cone axis
        int8_t cone_cutoff;        // cos(angle/2) for backface culling
    };

    // Vertex structure for BudEngine (Must match what is written in BudAssetTool)
    struct Vertex {
        float position[3];
        float normal[3];
        float uv[2];
        float tangent[4]; // Optional, but good to have
    };

#pragma pack(pop)

	constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;

    // Structural constants for verification
    constexpr uint32_t MESH_HEADER_SIZE = 124;
    constexpr uint32_t MESH_HEADER_VERTEX_OFFSET = 60;
    constexpr uint32_t MESH_HEADER_SUBMESH_COUNT_OFFSET = 20;
    constexpr uint32_t SUBMESH_DESCRIPTOR_SIZE = 44;

} // namespace bud::asset
