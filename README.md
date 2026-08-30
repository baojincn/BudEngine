# Bud Engine

A full fiber-based task driven lightweight 3D Game Engine with Nanite-style GPU-Driven Virtual Geometry and Modern Vulkan Rendering Pipeline.

---

## Key Features

### Core Architecture & Stability

* **Modern C++23 Standard**: Built with the latest language features (Concepts, std::print, designated initializers, user-defined literals `_m`, `_cm`, `_mm`, `_s`, `_mps`) ensuring high performance, type safety, and physical unit consistency (`1.0f Unit = 1.0 m`).
* **Robust Architecture**: Traditional Header/Source (`.hpp`/`.cpp`) structure ensures maximum compiler compatibility and stability across MSVC/Clang/GCC.
* **Fiber-Based Task System**: A lightweight, multi-threaded job system (`TaskScheduler`) for high-performance parallel processing.
  * *Why Fibers (Stackful Coroutines) over C++20 Stackless Coroutines?* In game engine architecture, tasks often need to yield execution from deep within complex, traditional C++ call stacks (e.g., waiting for physics constraints or asset loads inside nested functions). Stackless coroutines suffer from the "function color" problem, forcing every function in the chain to explicitly become a coroutine (`co_await`), which is highly invasive. Fibers allow transparent, arbitrary-depth suspension and full call-stack preservation, making them far more suitable for integrating large game subsystems and third-party libraries without polluting the codebase.
* **Dedicated Multi-Queue Architecture**:
  * Independent **DMA Transfer Queue**, **Async Compute Queue**, and **Graphics Queue** with timeline semaphores (`bud::rhi::TimelineSemaphore`) for lock-free cross-queue ownership transfers and asynchronous streaming.
* **Game Logic Decoupling**: Application layer is separated from the core engine using dependency injection (`GameApp`), allowing the engine to run as a standalone executable or an embedded library.
* **Integrated Profiling & GPU Diagnostics**:
  * Full CPU instrumentation using **Tracy Profiler** for real-time micro-profiling and frame budget monitoring.
  * Integrated **NVIDIA Nsight Aftermath** for capturing automated GPU crash dumps (TDRs, device lost diagnostics).
  * Robust **RenderDoc** and **Nsight Graphics** capture support with deterministic thread pinning.

---

### GPU-Driven Virtual Geometry Pipeline (Nanite-Style)

* **Dual Virtual Geometry Rasterization Paths**:
  * **Modern Mesh Shader Path**: Native Vulkan Task & Mesh Shader amplification (`visibility.task` + `visibility.mesh` via `VK_EXT_mesh_shader`).
  * **Compute Indirect MultiDraw Path**: GPU-driven cluster culling and MultiDrawIndexedIndirect rasterization (`page_emit.comp` + `cluster_cull.comp` + `visibility_indirect.vert/frag`).
* **GPU Hierarchy DAG Traversal (`HierarchyTraversalPass`)**:
  * Top-down GPU DAG LOD traversal (`hierarchy_traversal.comp`) evaluating projected screen-space pixel error against configurable thresholds (`lod_error_threshold_px`).
  * Supports dual projection models: Perspective error metric for main camera view and Orthographic error metric for shadow cascades.
* **Two-Phase Hi-Z Occlusion Culling**:
  * Cluster-level occlusion culling testing against the hierarchical depth pyramid (Hi-Z).
  * Two-phase GPU occlusion loop with coarse LOD prepass and atomic cluster bitmask tracking (`page_cluster_mask`).
* **Virtual Geometry Paging & Non-Blocking Streaming**:
  * Uniform 128KB binary memory pages (`bud::asset::VG_PAGE_SIZE = 131,040 Bytes`) streamed asynchronously via `bud::io`.
  * **Bindless GPU Page Pool & Page Table**: Global GPU storage buffer partitioned into 128KB slots with indirect SSBO Page Table indexing, enabling zero-rebind single-buffer rendering across massive scenes.
  * **Compact Geometry Packing**: 12-bit quantized position streams, packed snorm8 normals/tangents, and half-float UVs for minimal memory footprint and high memory bandwidth efficiency.
* **Multi-View VG Shadow Rasterization**:
  * Direct pure-depth Task/Mesh shader rasterization for all Cascaded Shadow Map layers, ensuring identical geometry fidelity between primary visibility and shadow passes.

---

### Rendering & Shading

* **Unified Visibility Buffer Architecture**:
  * Geometry-shading decoupled deferred pipeline: `Visibility Pass` encodes `(Instance ID, Cluster ID, Material ID, Depth)` into a 32-bit integer target (`VisibilityBuffer`), resolved in a single full-screen material resolve pass.
* **Physically Based Rendering (PBR)**:
  * Full Cook-Torrance BRDF with GGX Normal Distribution, Smith Geometry Shadowing, and Schlick Fresnel.
  * Integrated hemispheric sky/ground irradiance and emissive material contributions.
* **Cascaded Shadow Maps (CSM)**:
  * 4-Cascade shadow mapping with Log-Linear split depths and rotation-invariant spherical distance selection.
  * 16-tap Poisson disk Percentage-Closer Filtering (PCF) with adaptive world-space kernel normalization and smooth inter-cascade blending.
* **Screen-Space Reflections (SSR)**:
  * Hierarchical screen-space raymarching with coplanar self-intersection rejection (`dot(N_hit, N) > 0.85`), roughness cutoffs, and temporal reprojection sampling against history color.
* **Screen-Space Global Illumination (SSGI)**:
  * Cosine-weighted hemisphere diffuse bounce evaluation, spatial cross-bilateral edge-preserving filtering, and YCoCg variance-clamped temporal accumulation.
* **Ambient Occlusion Pipeline**:
  * Comprehensive screen-space ambient occlusion supporting **GTAO** (Ground Truth Ambient Occlusion) and **SSAO** with temporal accumulation and bilateral blur.
* **Modern Tone Mapping & Color Pipeline**:
  * ACES filmic tonemapping with sRGB gamma correction.

---

### Asset Pipeline (`BudAssetCompiler` & `BudAssetImporter`)

* **Multi-Format 3D Asset Importers**:
  * High-performance loaders for **FBX**, **glTF 2.0**, and **OBJ** with automatic normal/tangent generation and materials extraction.
* **Companion Alpha Mask Auto-Merging**:
  * Integrated `bcdec` for DDS BC1-BC7 block decompression; automatically detects and merges companion alpha masks (`_D.dds` + `_A.TGA`, `_diff` + `_mask`) into unified RGBA textures.
  * Automatic `AlphaMode::Mask` cut-off detection and double-sided material assignment for dense foliage.
* **Virtual Geometry Pre-processing & Baking**:
  * Cluster partitioning (`meshoptimizer`), METIS graph-partitioned cluster grouping, and DAG level generation baked into unified `.budasset` containers with `.budbulk` streaming payloads.

---

## Showcase

### Sun Temple (Virtual Geometry + SSGI + SSR + CSM)
![](samples/screenshots/sun_temple.png)

### Virtual Geometry Cluster Debug Visualization
![img](samples/screenshots/cluster_debug.png)

### Virtual Geometry Cluster Wireframe View
![](samples/screenshots/cluster_wireframe_debug.png)

### Cascaded Shadow Maps (CSM) Debug Overlay
![](samples/screenshots/csm_shadow_debug.png)

---

## Planned Features

* **Advanced Shadowing**:
  * Hybrid Mesh Distance Field (SDF) shadows for long-range terrain and structures (0-30m CSM / 30m+ SDF).
  * Virtual Shadow Maps (VSM) with physical page pooling and static caching.
* **Global Illumination**:
  * Distance Field Ambient Occlusion (DFAO) and Surfel-based indirect lighting.
* **Neural Rendering & AI Supersampling**:
  * ONNX Runtime integrated neural supersampling and temporal denoising.
* **Virtual Texture Streaming**:
  * Sparse binding and page-table management for ultra-high-resolution textures.
