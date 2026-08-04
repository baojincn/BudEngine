# Engine Architecture Notes

This document tracks high-level architectural decisions, design patterns, and structural rules for BudEngine.

## Architectural Design: Unique Ownership vs. Global State (Singleton)

In older or simpler engine architectures, core systems like `TaskScheduler`, `Renderer`, or `AssetManager` were often implemented as Global Singletons (e.g., `TaskScheduler::get().spawn(...)`). 
However, modern C++ (and this engine) heavily favors **Unique Ownership** and **Dependency Injection** over Singletons.

### 1. The Problem with Singletons
* **Hidden Dependencies:** When a class calls `TaskScheduler::get()`, the dependency is hidden within the source code. You cannot easily see what systems a class relies on by looking at its header.
* **Initialization Order Hell:** In C++, the initialization and destruction order of global/static variables across different translation units (.cpp files) is undefined context (the "Static Initialization Order Fiasco"). If the `Renderer` singleton relies on the `TaskScheduler` singleton during destruction, the engine might crash on exit depending on unpredictable compiler behavior.
* **Testing & Multi-instance:** Singletons make it impossible to spin up two completely isolated engine instances (e.g., for split-screen co-op, or running a headless server alongside a local client) because they share the same global state.

### 2. The Modern Approach: Unique Ownership & Dependency Injection (DI)
In `bud.engine.hpp`, you see the modern approach:
```cpp
class BudEngine {
private:
    // 1. Explicit, deterministic lifetimes via RAII & unique_ptr
    std::unique_ptr<bud::threading::TaskScheduler> task_scheduler;
    std::unique_ptr<bud::graphics::RHI> rhi;
    std::unique_ptr<bud::io::AssetManager> asset_manager;
    std::unique_ptr<bud::graphics::Renderer> renderer;
};
```
* **Deterministic Lifetime:** `unique_ptr` guarantees that these systems are destroyed in the exact reverse order of their declaration in the class. We *know* `renderer` will be destroyed before `rhi`, preventing crashes.
* **Clear Interfaces (Dependency Injection):** When `Renderer` needs the `TaskScheduler`, it is explicitly passed via the constructor: `Renderer(RHI*, AssetManager*, TaskScheduler*)`. Anyone reading the `Renderer` header instantly knows exactly what subsystems it needs to function.
* **Scalability:** By avoiding global state, we leave the door open to instantiate multiple `BudEngine` objects safely in the same process domain.

By strictly disabling copying on our core managers (`= delete`) and wrapping them in `std::unique_ptr` within a master `BudEngine` class, we perfectly emulate the "there is only one" intent of a Singleton, but with 100% thread-safety, deterministic destruction, and clean architectural boundaries.

## Rendering Architecture Rollout Status

To keep architecture and implementation aligned, the rendering pipeline is rolled out incrementally.

### Current implemented scope
- Stage 1 (`CPU Macro-Culling`) is implemented.
- Stage 2 (`GPU Instance-Culling`) foundation (Z-prepass + Hi-Z) is implemented.

### Planned next scope
- Stage 1.5: `Asset & Shader Toolchain` (`cgltf` parsing, HLSL->SPIR-V via DXC) is in progress.
- Stage 3: `GPU Meshlet/Micro-Culling` (high-end profile) is planned.
- Stage 4: `Indirect/Visibility-Buffer Dispatch` is planned.
- Stage 5: `Neural Rendering` (AI denoise + neural super-resolution) is in progress.

### Responsibility boundaries (unchanged)
- `BudEngine`: Orchestration, OS window events, and systems lifecycle.
- `RHI`: Device, swapchain, Vulkan sync primitives, and pure GPU resource handles (VMA).
- `Renderer`: Culling algorithms, materials, and draw dispatch.
- **`RenderGraph` (Owned by Renderer):** Acts as the central data-flow orchestrator. It manages pass dependencies, transient GPU memory aliasing, and automated image/buffer barrier generation.

Detailed data flow and pass-level notes are tracked in `doc/Graphics.md`.

## System-level Rendering Pipeline

BUDEngine architecture specification: triple-funnel culling and zero-copy ML interoperability.

Architecture topology: triple-funnel data flow.

### Rendering route model

The renderer is organized as a dependency graph at the pass level and a lifetime graph at the memory level.

1. **Pass execution model**
   - Render passes are authored as a `DAG`.
   - The `RenderGraph` resolves dependencies with a topological pass and emits a linear `FIFO` command stream for hardware execution.
   - Pass order must be expressed explicitly through resource dependencies, not by relying on insertion order.

2. **Transient memory model**
   - Intermediate buffers and textures are managed by exact lifetime analysis.
   - The transient resource pool should favor `LIFO` reuse for short-lived resources to improve cache locality and reduce allocation churn.

3. **Parallel branch rule**
   - `CSM shadow` is a parallel branch relative to the main camera path.
   - It should be constructed before the meshlet culling branch is compiled, then rejoined only where `Main Lighting` consumes the shadow map.

Host / CPU side
1. CPU frustum culling
   - Perform coarse distance and macro backface culling.
   - Result: approximately 100,000 surviving instance IDs.
   - Upload surviving instance IDs to GPU memory using ReBAR or write-combining for low-latency transfer.

Device / GPU side — constructing the occlusion base
2. Compute shader: Heuristic select
   - Parallel scoring using metrics such as screen area and distance squared.
   - Branch A: select the top 500 VIP occluders for prioritized processing.
     3. Draw pass: Z-prepass (render only the top 500 occluders).
     4. Compute shader: build Hi-Z (HzB) pyramid from the depth produced by the Z-prepass.
     - Result: a single HzB texture mipmap chain that is the authoritative depth source for this frame.
   - Branch B: the remaining ~99,500 instances are placed into a pending buffer for further per-instance visibility checks.

Device / GPU side — triple-funnel extreme culling
Funnel 1 — Macro culling (instance level)
5. Compute shader: instance HzB culling
   - Sample the Hi-Z mipmap chain to quickly reject fully occluded instances.
   - Result: reduce to roughly 20,000 surviving instances for the next stage.

Funnel 2 — Micro culling (meshlet level)
6. Compute shader: meshlet expansion and culling
   - Expand per-instance meshlet ranges and perform frustum plus Hi-Z tests per meshlet.
   - Result: produce a list of absolutely visible meshlet IDs written into a dispatch indirect buffer.

   - `CSM shadow` must remain an independent producer branch and not be serialized into the meshlet branch unless the main lighting pass explicitly consumes its output.

Funnel 3 — Nano culling (triangle level)
7. Mesh shader (triggered by indirect draw)
   - Perform fine-grained operations: backface culling, subpixel culling, and micro frustum clipping.
   - Compact local indices using subgroup ballot operations to tightly pack work and avoid redundant triangles.
   - Result: pure triangle primitives with zero overdraw entering the rasterizer.

8. Rasterizer and fragment shader: final pixel shading (G-buffer rendering).

### Detailed execution specification and ML data interop protocol

I. Core rendering features
1. The pipeline is GPU-driven to minimize CPU-side draw call overhead.
2. The Hi-Z buffer is rebuilt every frame to avoid temporal reprojection artifacts and disocclusion holes.
3. The pipeline performs hierarchical culling from instance level down to triangle level so only physically visible triangles reach the rasterizer.

II. ML zero-copy passthrough protocol (GPU-to-GPU interop)
To support large-scale reinforcement learning and ML workloads, the engine supports direct VRAM sharing between Vulkan and CUDA/PyTorch without CPU roundtrips.

1. VRAM allocation (Vulkan side)
   - When creating allocations for buffers or images that will be shared with ML backends, attach `VkExportMemoryAllocateInfo` and request an appropriate OS handle type (for example, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`).

2. Handle export (OS bridge)
   - Use `vkGetMemoryWin32HandleKHR` (Windows) to obtain a native HANDLE for the Vulkan memory allocation.

3. Memory takeover (CUDA / PyTorch side)
   - Import the external memory in CUDA using `cudaImportExternalMemory` with the provided native handle.
   - Use the resulting CUDA pointer or device pointer region with LibTorch APIs (for example, `torch::from_blob`) to construct a `torch::Tensor` on the CUDA device without copying.

4. Cross-API synchronization
   - Export a Vulkan semaphore to the OS and import it into CUDA as an external semaphore.
   - Before CUDA/PyTorch reads the shared memory, ensure the CUDA stream calls `cudaWaitExternalSemaphoresAsync` on the imported semaphore so that Vulkan writes are visible and complete. This prevents dirty reads and enforces correct ordering.

Refer to `doc/Graphics.md` for pass-level implementation notes and shader mappings.

