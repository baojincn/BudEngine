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
- Produces a compact visible instance list for downstream rendering.

**Benefit**
- Early rejection of out-of-frustum objects.
- Lower draw preparation cost and lower downstream GPU candidate count.

### Stage 2: GPU Instance-Culling (Planned)
**Status:** Planned

- Z-prepass (major occluders).
- Hi-Z/depth pyramid build.
- GPU instance occlusion culling to a survived instance buffer.

### Stage 3: GPU Meshlet/Micro-Culling (Planned, high-end profile)
**Status:** Planned

- Instance-to-meshlet expansion with LOD-aware dispatch.
- Meshlet frustum + normal-cone/backface + Hi-Z occlusion culling.

### Stage 4: Raster Dispatch (Planned)
**Status:** Planned

- Build indirect draw commands from survived buffers.
- Execute through `vkCmdDrawIndexedIndirect`.
- Optional visibility buffer route.

### Stage 5: Neural Rendering (Planned)
**Status:** Planned

- Optional AI denoise pass.
- Neural super-resolution from low-res `color/depth/motion vectors` (and history/exposure when enabled).
- Post-process and present to swapchain.

### Platform Routing Strategy
- **Mobile/TBDR:** CPU-heavy conservative path, strict bandwidth and thermal control.
- **Mainstream PC/Console:** GPU instance-culling as the default path.
- **High-end:** meshlet path + neural rendering path enabled.
