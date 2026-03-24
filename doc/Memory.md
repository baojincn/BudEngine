# Memory Management Notes

This document tracks decisions regarding memory allocators, object lifecycles, and resource management strategies in BudEngine.

## Core Concept: RAII vs. Smart Pointers in Low-Level Engine Systems

In modern C++ engine development, it's a common rule to "always use smart pointers." However, in highly performance-critical systems like a fiber-based Task Scheduler, you might notice the use of raw pointers combined with manual `delete` in the destructor.

It is important to clarify a misconception: **Smart pointers are just one standard library implementation of the RAII (Resource Acquisition Is Initialization) paradigm**. Manually managing raw pointers inside a class and cleaning them up in the destructor is *also* RAII. The class itself acts as the RAII wrapper.

### 1. Why Raw Pointers over Smart Pointers in `TaskScheduler`?

* **Lock-Free Data Structures:**
  Components like `LockFreeFiberPool` rely heavily on atomic Compare-And-Swap (CAS) operations (`compare_exchange_weak`). CAS operates most efficiently on raw pointer-sized integers or pointers. While C++ provides `std::atomic<std::shared_ptr>`, its underlying implementation often involves spinlocks or atomic reference counting. In an ultra-high-performance Job System, this overhead is absolutely unacceptable. `std::unique_ptr` cannot be copied and doesn't natively support raw CAS without constant `.release()` and `.reset()`, breaking its semantic purpose.
* **Intrusive Data Structures:**
  The `Fiber` struct contains internal pointers like `next_waiting` and `next_pool`. It acts as its own linked-list node. In systems where elements are enqueued/dequeued millions of times per frame, modifying raw pointers directly is extremely fast and cache-friendly. The control-block overhead of smart pointers would severely degrade performance here.
* **Extreme Ownership Transfers:**
  Fibers wildly shift ownership between the main thread, queues, active execution, wait lists, and worker threads (task stealing). Doing this with smart pointers requires pervasive `std::move()` usage, generating semantic noise. If `std::shared_ptr` were used, the constant atomic modification of cross-thread reference counts would cause massive "False Sharing" and cache line invalidations.

### 2. When to use Which?

**Use Smart Pointers (`std::unique_ptr` / `std::shared_ptr`) - The 95% Rule:**
* Application layer / Gameplay logic (entities, UI components).
* Single-allocation high-level engine subsystems (e.g., the Engine owning a `std::unique_ptr<Renderer>`).
* Complex, non-performance-critical shared ownership scenarios.

**Use Raw Pointers + Manual RAII Encapsulation - The 5% Rule:**
* Bottom-layer engine infrastructure: Task Schedulers, Memory Allocators, ECS core arrays.
* Interfacing with C-APIs (Vulkan object handles, SDL Windows, Assembly context switches).
* Lock-Free / Wait-Free algorithms requiring byte-level memory manipulation.
* Extreme hot-paths where custom object pooling (like `fiber_pool`) or arena allocators completely replace standard `new/delete`.

### 3. Three Classic Applications of RAII in the Engine

In `bud.threading.cpp`, RAII is manifested in three distinct ways:

**1. Managing Locks (Synchronization Primitives RAII)**
```cpp
void TaskScheduler::spawn_on_thread(...) {
    // ...
    // RAII manages the Mutex here
    std::lock_guard lock(workers[thread_index]->pinned_mtx);
    workers[thread_index]->pinned_queue.push_back(f);
} // <--- Scope ends, 'lock' object is destroyed, its destructor automatically unlocks the Mutex
```
**Analysis:** This is the most typical non-memory resource RAII. Without `lock_guard`, if `push_back` throws an exception before `mutex.unlock()` is called, the next thread will deadlock. `std::lock_guard` perfectly solves this by tying the lock state to the stack frame.

**2. Managing Threads (System Resources RAII)**
In the `TaskScheduler` constructor, C++20's `std::jthread` is used:
```cpp
workers[i]->thread = std::jthread([this, i](std::stop_token st) {
    worker_loop(i, st);
});
```
**Analysis:** The older `std::thread` did not enforce strict RAII. If you didn't explicitly call `join()` or `detach()` before a `std::thread` was destroyed, the program would crash via `std::terminate`. C++20's `std::jthread`, however, implements perfect RAII: when destroyed, it automatically signals cancellation via a stop_token and safely `join()`s the thread, ensuring zero resource leakage.

**3. Implementing a Resource Wrapper (The Class as an RAII Container)**
`TaskScheduler` itself acts as a massive RAII container:
```cpp
TaskScheduler::~TaskScheduler() {
    stop();
    // When leaving scope, the Scheduler automatically cleans up underlying Workers and Fibers
    for (auto w : workers) {
        for (auto f : w->pinned_queue) delete f; // Free Fibers
        delete w;                                // Free Workers
    }
    // Clean up residual Fibers in the lock-free pool
    Fiber* f = nullptr;
    while ((f = fiber_pool.pop()) != nullptr) {
        delete f; 
    }
}
```
**Analysis:** Engine development frequently requires building low-level resource managers. `TaskScheduler` provides absolute RAII safety to the higher level (`BudEngine`): as long as `BudEngine` is destroyed, `TaskScheduler` is destroyed, guarantees that all internal raw pointers it hoards are safely deallocated. This is the definition of "encapsulating the lifespan of raw low-level resources within the destructor of a high-level application class."

### 4. RAII vs. Garbage Collection (GC) in Game Engines

Why stick to RAII when GC exists?
* **GC** only manages *memory* and does so non-deterministically, leading to frame stutters/spikes. 
* **RAII** is deterministic. When a scope ends, destructors run immediately. This allows it to manage *any* resource instantly, such as closing file handles, unlocking Mutexes (`std::lock_guard`), freeing Vulkan Images/Buffers, or releasing Thread pools (`std::jthread`).

### 5. Best Practices for Custom RAII Classes

When designing a top-level RAII wrapper like `TaskScheduler` that manually manages raw pointers, it must adhere to the **Rule of 3 / 5 / 0**. 
Since `TaskScheduler` is a unique system manager, it should explicitly disable copying to prevent double-free crashes:

```cpp
class TaskScheduler {
public:
    TaskScheduler(size_t n);
    ~TaskScheduler();

    // Disable copy semantics to guarantee RAII safety
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
};
```
## 6. GPU Memory Management (VRAM)

Managing GPU memory requires entirely different paradigms than CPU memory. BudEngine strictly avoids manual `vkAllocateMemory` calls in favor of a dedicated memory allocator and graph-based lifetimes.

### 6.1 Vulkan Memory Allocator (VMA)
BudEngine utilizes AMD's Vulkan Memory Allocator (VMA) under the hood of the `RHI`. 
* **Reason:** Vulkan has strict limits on the maximum number of memory allocations (often 4096). Attempting to allocate a raw `VkDeviceMemory` block for every single texture or buffer will cause crashes. VMA allocates large blocks of VRAM and sub-allocates them to engine resources efficiently.

### 6.2 Render Graph & Transient Memory Aliasing
Modern rendering pipelines (like BudEngine's multi-stage GPU culling and neural rendering) generate dozens of intermediate Render Targets (e.g., Z-Prepass Depth, Hi-Z Mipmaps, Culling bitmasks).
* **The Strategy:** The `RenderGraph` analyzes the dependency chain of passes. If Pass C runs after Pass A finishes, and Pass C doesn't need Pass A's output, they can share the exact same physical VRAM address. This is called **Transient Memory Aliasing**, and it is critical for keeping 4K rendering within reasonable VRAM budgets.

### 6.3 CPU-GPU Data Streaming (Ring Buffers)
When streaming data (like Instance Matrices or Material params) from the CPU to the GPU, BudEngine uses **Per-Frame Ring Buffers**.
* **Rule:** If the Swapchain has 3 images in flight, dynamic GPU buffers must be allocated with a size of `DataSize * 3`.
* **Execution:** The CPU only maps and writes to the memory segment corresponding to the `current_frame_index`. This prevents the CPU from overwriting data that the GPU is currently reading for a previous frame in flight, completely eliminating tearing and race conditions.

---

# Memory Management Design — BudEngine

This section describes the specific memory management architecture used by the Vulkan RHI in BudEngine.

## 7. Goals

- Provide efficient CPU->GPU upload paths for frequent small updates (uniforms, instances, UI, dynamic geometry).
- Provide long-lived device-local allocations for large GPU resources (vertex/index buffers, SSBOs, transfer-only images).
- Minimize per-frame Vulkan object churn (avoid creating/destroying VkBuffer each frame) using sub-allocation and ring-staging.
- Keep ownership clear and safe across CPU/GPU asynchronous execution with a simple deferred-release mechanism.
- Provide a backward-compatible fallback when the unified allocator is not available.

## 8. High-level Architecture

- **`VulkanRHI` (high-level RHI)**: Exposes resource creation APIs used by renderer and engine code. It delegates to the allocator for actual memory operations.
- **`VulkanMemoryAllocator`**: The VMA-backed allocator implementation. It manages:
  - Ring/linear staging pages for CPU-visible uploads (per-frame slots).
  - Dedicated VMA allocations for device-local and persistent buffers.
  - Deferred-free lists to safely reclaim resources once the GPU is done with them.
- **`VulkanBuffer` / `VulkanTexture`**: Lightweight runtime wrappers holding Vk handles and allocation metadata (allocation, mapped pointer, size, offset, ownership flag).

## 9. Resource Categories

- **Staging (CPU-visible ring/linear pages)**
  - Intended for short-lived uploads: uniforms, instance data, UI vertex/index updates, small dynamic meshes, meshlet uploads.
  - Implemented as a vector of `VmaLinearPage` — one or more pages per in-flight frame slot.
  - Supports sub-allocation (offset + mapped_ptr) to avoid creating a new VkBuffer per upload.
  - Each page is reclaimed only after the GPU work that references it is known complete.
  - When sub-allocation cannot satisfy a request, allocator falls back to creating a mapped VMA buffer.
- **Device-local GPU buffers**
  - Allocated via VMA for `DEVICE_LOCAL` usage.
  - Used for long-lived geometry, SSBOs, readback buffers (if mapped differently), etc.
- **Persistent mapped buffers**
  - Allocated and kept mapped for frequent CPU writes (persistent uniform buffers or streaming regions).

## 10. Implementation Details

### 10.1 Allocation Strategies

- **Ring/linear staging**
  - The allocator exposes `alloc_staging(size, alignment)` which returns a `BufferHandle` containing: internal_state, offset, size and mapped_ptr.
  - Allocation tries to place the request into the current frame's linear page. If insufficient space, will either move to next page or create a temporary mapped buffer.
  - Design requires that the ring size (number of pages) equals or exceeds the number of frames in flight to guarantee safe reuse.
- **GPU/persistent allocations**
  - `alloc_gpu(size, usage_state)` and variants create dedicated VMA buffers and return handles with ownership metadata.
- **Fallback**
  - `VulkanRHI::create_upload_buffer` prefers `memory_allocator->alloc_staging(...)`. If allocator is not present, it falls back to creating a mapped VMA buffer via `vmaCreateBuffer`.

### 10.2 Deferred-Free Mechanism

- **Purpose**: Avoid destroying underlying Vulkan resources while GPU may still reference them.
- **Implementation**:
  - Two per-frame-slot containers: `deferred_free_buffers` and `deferred_free_textures` (vectors of vectors).
  - `defer_free(handle, frame_index)` records the resource into a slot using `index = frame_index % deferred_list_size`.
  - During allocator's per-frame reclamation step, the allocator iterates the entries for the slot corresponding to the completed frame and destroys VMA buffers, clears lists, and resets staging page for reuse.
- **Ownership Notes**:
  - Buffers owned by the allocator are destroyed by allocator during deferred-free reclamation (calls `vmaDestroyBuffer`).
  - Texture ownership must be explicit: currently textures are pushed to `deferred_free_textures` but actual destroy behaviour should be clarified (who frees VkImage, VkImageView, and deletes the wrapper?).

### 10.3 Synchronization and Lifetime Rules

- **Frames in Flight and Staging Reuse**:
  - The allocator must be configured with a deferred list length >= `max_frames_in_flight` (preferably `max_frames_in_flight + 1`) to avoid reuse while GPU still holds references.
  - When the CPU writes into a staging page and the command buffer referencing it is submitted, the frame index used to record the submission must be passed to `defer_free` / reclamation logic so the allocator knows when it is safe to reuse that page.
- **GPU Submission Ordering**:
  - Caller (VulkanRHI/renderer) is responsible to ensure that command buffers referencing staging or temporary buffers are submitted with fences/semaphores and that the frame index provided to allocator reclamation corresponds to the fence-completed frame.

### 10.4 API Surface (Important Functions)

- `alloc_staging(size, alignment)` -> `BufferHandle`
  - Returns mapped pointer and offset for CPU write; may return a dedicated mapped buffer if ring page cannot satisfy.
- `alloc_gpu(...)` -> `BufferHandle`
  - Creates device-local or otherwise persistent buffers.
- `alloc_persistent(...)` -> `BufferHandle`
  - For buffers that remain mapped/persistent.
- `defer_free(const BufferHandle&, frame_index)` / `defer_free(Texture*, frame_index)`
  - Record resource for safe destruction when frame is retired.
- `get_vma_allocator()` -> `VmaAllocator`
  - Expose underlying VMA allocator when direct VMA calls are required.

## 11. Integration & Reliability

### 11.1 Integration in RHI and Renderer
- Renderer code and higher-level passes should call `get_allocator()->alloc_staging(...)` for transient uploads.
- `VulkanRHI::create_upload_buffer` now prefers allocator-based staging; fallback exists for compatibility.
- `copy_buffer_to_image` signature was updated to accept a `buffer_offset` to support staging sub-allocations.

### 11.2 Failure Modes and Mitigations
- **Allocation failure for staging**: Allocator falls back to creating a temporary mapped VMA buffer mapping for the request.
- **Double-free / Ownership ambiguity**: Ensure ownership semantics are documented: allocator destroys buffers it allocated; higher-level code must destroy texture `VkImage`/`VkImageView` or transfer ownership explicitly to allocator.
- **Synchronization bugs**: Incorrectly sized deferred lists or wrong frame indices cause use-after-free or resource leaks. Mitigation: initialize deferred lists to `max_frames_in_flight` and assert in debug builds.

## 12. Performance Considerations

- Staging page size and alignment should be tuned to typical upload patterns (UI vs bulk meshlets). Larger pages reduce `vmaCreateBuffer` calls but increase memory waste.
- Marking staging `VkBuffers` with `VERTEX`/`INDEX` usage is convenient for dynamic UI but may have driver-specific performance characteristics — keep this use limited to short-lived, frequently-updated data.
- Avoid logging in hot allocation paths; use conditional debug logging or rate-limited logs.

## 13. Testing & Validation Checklist

1. Build with validation layers enabled; exercise frequent uploads and verify no validation errors.
2. Test with `max_frames_in_flight` = 1 and >1 to validate deferred-free indexing and reuse.
3. Force allocator-unavailable path to validate fallback behavior (`create_upload_buffer` fallback).
4. Run on multiple vendors (NVIDIA/AMD/Intel) to catch driver-specific issues with staging-as-vertex/index usage.
5. Tooling: use RenderDoc and Vulkan validation to verify no use-after-free and correct synchronization.

## 14. TODO / Open Items

- Clarify and implement texture ownership and destruction semantics for `defer_free(Texture*)`.
- Add unit/integration tests that simulate delayed GPU completion and ensure deferred-free reclamation occurs correctly.
- Consider exposing an explicit `allocator->tick(frame_index)` API that the RHI calls when a frame's fence signals, centralizing reclamation.
- Add more robust logging toggles to avoid runtime overhead in release builds.

---

Document generated by GitHub Copilot assistant on request; adapt details to actual allocator implementation before shipping.
