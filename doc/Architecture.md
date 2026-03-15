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
- The engine currently performs CPU-side instance visibility filtering before deeper GPU-side filtering.

### Planned next scope
- Stage 2: `GPU Instance-Culling` (Z-prepass + Hi-Z + occlusion culling) is planned.
- Stage 3: `GPU Meshlet/Micro-Culling` (high-end profile) is planned.
- Stage 4: `Indirect/Visibility-Buffer Dispatch` is planned.
- Stage 5: `Neural Rendering` (AI denoise + neural super-resolution) is planned.

### Responsibility boundaries (unchanged)
- `BudEngine`: orchestration and lifecycle.
- `RHI`: device/swapchain/synchronization ownership.
- `Renderer`: pass graph, culling algorithms, and draw dispatch.

Detailed data flow and pass-level notes are tracked in `doc/Graphics.md`.
