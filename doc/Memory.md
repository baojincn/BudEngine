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
