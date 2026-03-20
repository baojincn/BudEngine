# Graphics and Rendering Notes

This document tracks the rationale behind the rendering architecture, Vulkan API usage, and graphics optimizations.

## Architectural Design: Separation of RHI and Renderer

In modern engine architecture (similar to standards seen in Unreal Engine 5, Frostbite, etc.), the core Engine class often bypasses the `Renderer` to interact directly with the `RHI` (Render Hardware Interface) for specific tasks. This is a deliberate design choice based on the **Separation of Concerns** and data flow.

### 1. Hardware Lifecycle vs. Algorithmic Rendering
* **RHI (Hardware Abstraction):** The RHI is strictly concerned with the GPU device, memory allocation, command queues, sync primitives (Fences/Semaphores), and OS-level window integrations (Swapchain). It has zero knowledge of high-level concepts like "Materials", "Shadows", or "Meshes".
* **Renderer (Algorithm Abstraction):** The Renderer handles graphical logic. It organizes the Render Graph, issues culling, arranges draw calls, and executes passes (Z-Prepass, Deferred Lighting, CSM). It should not care about operating system events.

### 2. Avoiding the "God Class" Anti-Pattern
If the `Renderer` were to handle everything graphical, it would quickly bloat into a "God Class." 
For example, when the user resizes the OS window, the `Window` captures the OS event. Routing this OS-level event through the `Renderer` just to tell the GPU to recreate a Swapchain breaks the single-responsibility principle. The `Renderer`'s job is to paint a scene into a target, not to negotiate with the OS about window dimensions.

### 3. The Engine as the "Orchestrator"
The `BudEngine` master class acts as the orchestrator. It manages the lifecycle of all underlying infrastructure.
* **Window Resizing:** `BudEngine` detects the resize event via the platform `Window` and directly commands the `RHI` to `resize_swapchain()`. The `Renderer` is completely unaware of this happening; it simply receives a new `SceneView` with updated viewport dimensions on the next frame.
* **GPU Synchronization:** During teardown or resizing, `BudEngine` calls `rhi->wait_idle()`. Managing hardware thread states and safe memory destruction is purely an infrastructure task, entirely decoupled from the rendering algorithms.

### 4. AAA Industry Precedents (e.g., Unreal Engine 5)
This exact data-flow is ubiquitous in AAA engines. In UE5, the UI/Windowing system (Slate) directly invokes RHI commands (`RHIUpdateViewport`) to rebuild swapchains when window sizes change. Similarly, global teardown sequences use global RHI flush commands (`FlushRenderingCommands()`) to sync the GPU, completely bypassing the high-level `FDeferredShadingSceneRenderer`.

Following this pattern ensures that the RHI remains a pure graphics API wrapper, the Renderer remains a pure graphics algorithm executor, and the Engine safely glues them together based on OS and Game thread data flows.

## Shader Toolchain and Cross-Platform Infrastructure

To support multiple backend APIs (Vulkan, DX12, Metal) without maintaining redundant shader codebases, BudEngine enforces a unified shader compilation pipeline.

* **Single Source of Truth (HLSL):** All shaders (Vertex, Fragment, Compute, Mesh/Task) are authored exclusively in HLSL.
* **Compilation Pipeline:** Microsoft's DirectX Shader Compiler (DXC) is used to compile HLSL directly into SPIR-V. For future Metal support, SPIRV-Cross will be utilized to translate SPIR-V into MSL (Metal Shading Language).
* **Automated Reflection:** SPIR-V reflection is used to automatically generate C++ Descriptor Set Layouts and Push Constant bindings. Hardcoding descriptor layouts in C++ is strictly prohibited to prevent pipeline mismatches.

## Data Synchronization: CPU-GPU Memory Management

When streaming dynamic per-frame data (e.g., Object Transforms, Material updates) from the CPU to the GPU, direct mapping without synchronization leads to race conditions and frame tearing.

* **Per-Frame Ring Buffers (Multi-buffering):** All dynamic CPU-to-GPU `StructuredBuffer` and `UniformBuffer` allocations are multiplied by the Swapchain image count (typically N=2 or 3). 
* **Write-Safe Data Streaming:** The CPU always writes to the memory block designated for the *current* in-flight frame, ensuring the GPU can safely read the *previous* frames' data without stalling or memory corruption.
* **Transient Memory Aliasing (Render Graph):** For intermediate render targets (e.g., Hi-Z pyramids, G-Buffers), the Render Graph will alias physical GPU memory allocations (via VMA) across disjoint passes. This prevents VRAM exhaustion when rendering at 4K resolutions with deep compute chains.

## Graphics & Vulkan: Optimizing GPU Synchronization

### The Wait Stage Context (`pWaitDstStageMask`)
When submitting a command buffer to a Vulkan queue (`vkQueueSubmit`) linked to swapchain presentation, passing the `image_available_semaphore` (acquired from `vkAcquireNextImageKHR`) is mandatory. Crucially, Vulkan requires you to specify exactly *where* in the pipeline the GPU should pause and wait for this semaphore via `pWaitDstStageMask`.

**The Inefficient Approach:** `VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT`
This is a common beginner mistake. It tells the GPU to halt processing completely. The command buffer will sit idle, doing absolutely zero work until the presentation engine (OS/monitor) hands the swapchain image back. 

**The Optimized Approach: Deep Pipeline Parallelism**
```cpp
// Specify only the color attachment stage
VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
```
By pinpointing the color attachment output stage, we are enabling the GPU to start executing the command buffer immediately. The GPU can perform vertex fetching, run vertex shaders, and do depth testing for the incoming frame *concurrently* while waiting for the monitor/OS to release the image. 

The pipeline will only stall at the very end—the `Color Attachment Output` stage—because that is the exact physical moment the hardware needs the memory address of the swapchain image to write the final pixels. This overlap maximizes GPU utilization and drastically improves framerates.

## BudEngine Multi-stage Rendering Path (Implementation Status)

This section records the current progress of the multi-level culling/rendering design.

### Stage 1: CPU Macro-Culling (Implemented)
**Status:** Implemented

**Current result**
- CPU-side frustum culling at instance level (LBVH-oriented broad filtering).
- **Screen-Space Area Heuristic:** The CPU LBVH traversal dynamically calculates the estimated screen-space projection area of bounding boxes. Instances are automatically categorized and split into an `Occluder List` (large objects) and a `Detail List` (small objects).
- Produces compact visible instance lists for downstream rendering.

**Benefit**
- Early rejection of out-of-frustum objects.
- Lower draw preparation cost and prevents downstream GPU compute shaders from processing irrelevant global data.
- Automates the extraction of major occluders without manual artist tagging.

### Stage 2: GPU Instance-Culling (Implemented)
**Status:** Implemented

BudEngine implements a dual-pipeline occlusion strategy to balance extreme precision with robust compatibility.

#### Path A: RL-Driven Concurrent Hi-Z (Innovation Path)
This path eliminates the temporal-lag artifacts common in traditional engines by using Reinforcement Learning to identify occluders for a current-frame Z-Prepass.
1.  **RL Selection (CPU)**: The `ml_perception` module utilizes a trained RL model to select a minimal set of "Effective Occluders" (objects with the highest occlusion potential based on camera state).
2.  **Occluder Z-Prepass (GPU)**: Renders only the selected occluders to establish a high-confidence depth baseline.
3.  **Concurrent Hi-Z**: A Hi-Z pyramid is generated immediately from the current-frame Z-Prepass.
4.  **Detail Culling**: The entire "Detail List" (remaining objects) is tested against this *current-frame* Hi-Z.
*   **Benefit**: Zero-latency occlusion; no "popping" associated with previous-frame depth data; highly efficient for dense environments.

#### Path B: Heuristic-Driven Hi-Z (Fallback/Baseline)
A traditional heuristic approach used as a fallback for platforms where RL inference is unavailable or as a performance baseline.
1.  **Heuristic Filtering**: Uses screen-space area projection or simple distance-to-camera heuristics to categorize occluders.
2.  **Temporal Hi-Z**: Tests instances against the Hi-Z pyramid generated from the *previous frame's* complete depth buffer.
3.  **Benefit**: Low CPU overhead; standard industry behavior; useful for comparison with RL-driven results.

### Stage 3: GPU Meshlet/Micro-Culling (Planned, high-end profile)
**Status:** Planned

- Instance-to-meshlet expansion with LOD-aware dispatch.
- Meshlet frustum + normal-cone/backface + Hi-Z occlusion culling.

### Stage 4: Raster Dispatch (Planned)
**Status:** Planned

- Build indirect draw commands from survived buffers.
- Execute through `vkCmdDrawIndexedIndirect`.
- Optional visibility buffer route.

### Stage 5: Neural Rendering (In Progress)
**Status:** In Progress

- Optional AI denoise pass.
- Neural super-resolution from low-res `color/depth/motion vectors` (and history/exposure when enabled) running via Compute Shader inference.
- Post-process and present to swapchain.

### Platform Routing Strategy
- **Mobile/TBDR:** CPU-heavy conservative path (Stage 1 -> Direct Draw). Strict bandwidth and thermal control, skipping heavy GPU compute culling.
- **Mainstream PC/Console:** GPU instance-culling (Stage 1 -> Stage 2 -> Stage 4) as the default path.
- **High-end:** Meshlet path + neural rendering path (Stage 1 -> 5 full chain) enabled.

## Development Environment
The engine uses **Visual Studio 2026 (Professional)** as the primary toolchain. For Visual Studio Code, use the `Visual Studio 18 2026` generator in `CMakePresets.json` to ensure robust automatic discovery of the MSVC environment and Vulkan SDK, providing a "one-click" build experience.

## Asset Pipeline: Meshletization & Scene Workflow

BudEngine uses a dedicated asset pipeline to transform source data (glTF) into runtime-optimized formats.

### 1. Meshletization (.budmesh)
To support **Stage 3 (Cluster-level culling)**, static meshes are pre-processed into meshlets using `meshoptimizer`.
*   **Format**: Binary (.budmesh).
*   **Pipeline**: `glTF Source` -> `BudAssetTool (CLI)` -> `.budmesh`.
*   **Data Layout**: Optimized for direct GPU ingestion, containing Meshlet descriptors, Bounding Spheres (for culling), and Normal Cones (for backface culling).

### 2. Scene Description (.budmap)
Level layouts and entity metadata are stored in a human-readable format to facilitate debugging and version control.
*   **Format**: JSON (.budmap).
*   **Content**: Entity hierarchies, transforms, light configurations, and camera settings. 
*   **References**: Entities in `.budmap` refer to their geometric data via paths to `.budmesh` files.

### 3. BudAssetTool: The Offline Factory

`BudAssetTool` acts as the engine's "back-end processing factory," decouple high-level artistic assets (glTF, PNG, etc.) from the performance-critical runtime implementation.

#### Geometry Processing
*   **Meshletization**: Split meshes into cluster-sized chunks (Stage 3 bottleneck).
*   **Optimization**: Vertex cache optimization and overdraw reduction via `meshoptimizer`.
*   **LOD Generation**: Automated simplification and boundary-consistent meshlet slicing.

#### Texture & Material Processing
*   **Compression**: Transcode textures (JPG/PNG) into GPU-native formats (BC7 for PC, ASTC for Mobile) to minimize VRAM bandwidth and footprint.
*   **Mipmap Generation**: High-quality offline mip filtering.

#### Shader & Infrastructure Processing
*   **Offline Compilation**: Batch compile HLSL/GLSL to SPIR-V.
*   **Shader Reflection**: Analyze SPIR-V to automatically generate C++ binding headers (`.hpp`) and descriptor layout metadata (JSON), ensuring 100% sync between CPU and GPU code.
*   **Cross-Compilation**: Support for Metal (MSL) and DX12 (DXIL) translation for future multi-platform targets.

#### Scene & Map Baking
*   **Source vs. Runtime**: `.budmap` (JSON) is the human-editable source of truth; `.budmapb` (Binary) is the "cooked" version for high-speed production loading.
*   **LBVH Pre-construction**: Pre-construct Bounding Volume Hierarchies (LBVH) on disk for near-instant scene initialization.

### 4. Blender Integration (Artist Workflow)
Using **Pybind11**, the engine's core slicing and asset logic is exposed as a Python module. This allows a Blender plugin to:
*   Trigger `BudAssetTool` logic natively within the DCC environment.
*   Visualize meshlet boundaries, LOD transitions, and culling data directly on the artistic viewport for verification.

