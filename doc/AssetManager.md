# Asset Manager & Pipeline Architecture

## Overview
Bud Engine uses a **GPU-Driven Rendering** architecture with highly optimized **Hierarchical Culling (Mesh Shading / Meshlet Culling)**. To fuel this modern pipeline, the engine demands perfectly structured, spatially optimized asset data. 

The primary job of `BudAssetTool` is to ingest common 3D formats (via Assimp) and distill them into the highly optimized `.budmesh` binary format. However, standard Assimp processing flags are built around assumptions of legacy rendering pipelines, leading to severe incompatibilities with our architecture.

---

## Why We Bypass Assimp's Default Topological Processing

### 1. The Draw Call vs. Spatial Granularity Conflict
**Legacy Pipeline Goal:** Minimize CPU draw calls at all costs. Assimp's `aiProcess_PreTransformVertices` achieves this by baking all scene graph transforms and forcefully **merging all meshes that share the same material**. 
- _Example:_ In Sponza, this flag merges 393 perfectly isolated architectural chunks into 25 enormous material meshes (e.g., all brick walls across the entire level become a single mesh).

**Bud Engine Goal:** Maximize Spatial Granularity. Our rendering backend uses Frustum, Hi-Z Occlusion, and Meshlet Culling on the GPU. 
- If Sponza is merged into 25 giant material-based meshes, the Bounding Volumes (AABBs) would span the entire world. If the camera sees even one brick, the GPU is forced to process all bricks in the entire level.
- By **rejecting** `aiProcess_PreTransformVertices`, `BudAssetTool` manually preserves the original 393 strictly isolated Submeshes. This enables the engine to cleanly cull thousands of occluded structural chunks in zero time.

### 2. Manual Hierarchy Baking (`collect_instances`)
Because we forgo Assimp's automatic pre-transformations, `BudAssetTool` runs a custom recursive traversal (`collect_instances`) over `aiScene->mRootNode`. This guarantees:
1. **1:1 Spatial Mapping:** Every physical node in the modeling software remains structurally distinct.
2. **Proper Local Transforms:** The node's `mTransformation` is manually baked into the vertex `position` and tangent space `normals`, preparing the vertices for rapid World-Space processing without requiring complex CPU hierarchies at runtime.

### 3. Handling Negative Scaling and Winding Orders (Backface Culling Issue)
When `mTransformation` is manually baked, a critical edge case arises: **Negative Scaling / Mirroring**. 
Many modular assets (like Sponza's Lion Heads or Flagpoles) are modeled once but mirrored across axis using negatively scaled matrices.
- Multiplying vertices by a matrix with a **Negative Determinant** mathematically inverses the surface normals and the 2D projected winding order from Counter-Clockwise (CCW) to Clockwise (CW).
- If ignored, the GPU's **Backface Culling** will silently erase these mirrored geometries from the screen.

**The Fix:** 
`BudAssetTool` explicitly inspects the transformation determinant:
```cpp
bool needs_winding_flip = instance.transform.Determinant() < 0.0f;
```
If negative scale is detected, the triangle indices are aggressively swapped (`indices[1]` and `indices[2]`) prior to MeshOptimizer compaction. This perfectly resolves inverted normals and guarantees accurate front-facing visibility without relying on destructive Assimp flags.

---

## The BudMesh v3 Binary Format

The `BudAssetTool` outputs `.budmesh` (v3), an interlocking stream of carefully packed geometry:

1. **Header Block:** `BudMeshHeader` (124 Bytes, `#pragma pack(1)` locked for alignment). Contains all offsets.
2. **Vertex/Index Buffers:** Post-MeshOptimizer globally unified buffers ready for zero-copy upload.
3. **Meshlet Descriptors:** Spatial boundaries, cone cull data, and packed indices specifically formatted for Task / Mesh Shaders.
4. **SubMesh Descriptors:** Retains the 393 distinct instance chunks with tight CPU/GPU Hi-Z AABBs, material index routing, and offsets into the meshlet arrays.
5. **Texture Palette:** Zero-terminated collection of resolved texture filepaths that the engine's `AssetManager` automatically wires into the Bindless Textures array.
