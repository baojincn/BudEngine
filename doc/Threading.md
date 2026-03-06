# Threading and Job System Notes

This document tracks the design of the concurrent systems, fibers, and thread scheduling rules.

## Core Concept: Thread Affinity and Scheduling

In multi-threaded engine architecture, "Thread Affinity" dictates where specific workloads execute. This occurs at two distinct levels: inside the Engine's Job System (Task-Level) and at the Hardware/OS level (OS-Level).

### 1. Task-Level Thread Affinity (Job System)
In a standard work-stealing Job System, a task can be executed by *any* available worker thread. However, some tasks are strictly tied to OS limitations or specific thread-local states:
* **The Problem:** OS Windowing APIs (Win32, SDL) require message pumping and window handles to be manipulated *strictly* on the Main Thread. Legacy graphics APIs (like OpenGL) tie contexts to specific threads. 
* **The Solution:** The `TaskScheduler` uses `target_thread_index` and a mutually exclusive `pinned_queue`. If a task is pinned via `spawn_on_thread(0, ...)`, worker threads are explicitly forbidden from "stealing" it. Even if the fiber yields to wait on a counter and is later awakened, the wake-up logic explicitly routes it back to the target thread's `pinned_queue` instead of the general work pool.

### 2. OS-Level Thread Affinity (Hardware Level)
OS-Level affinity (via `SetThreadAffinityMask` on Windows, or `pthread_setaffinity_np` on Linux) forces the OS scheduler to bind a specific `std::thread` to a specific physical CPU core.
* **Why use it?** 
  1. **Cache Locality:** Prevents the OS from bouncing a worker thread between cores, keeping L1/L2 caches warm and reducing memory latency.
  2. **Console Development:** Consoles strictly partition cores (e.g., Cores 0-1 for OS, Cores 2-7 for the Title's game engine). Explicit thread affinity is mandatory to pass hardware certification.
* **The PC Gotcha:** Never hardcode OS thread affinity masks for general PC builds. PC hardware topologies vary wildly (from 4-core laptops to 64-core Threadrippers). Forcing specific affinity masks on unknown hardware configurations will likely cluster threads onto a single core or crash the engine, destroying performance.

### 3. Thread Priorities & Big.LITTLE (P-Cores vs E-Cores)
Modern CPUs (Intel 12th+ Gen, Apple Silicon, modern ARM) feature heterogeneous architecture: fast P-Cores and slow E-Cores. How do we ensure critical engine threads don't get stuck on slow E-Cores without manually hardcoding physical thread affinity?
* **Modern OS Schedulers:** Windows 11 (via Intel Thread Director) automatically analyzes thread behavior. It identifies heavy SIMD/math operations in foreground Window processes and natively promotes those threads to P-Cores. It dynamically demotes waiting or background threads to E-Cores.
* **Thread Priorities (The Safe "Affinity"):** Instead of strict hardware core pinning, flexible game engines use OS Thread Priorities to strictly guide the OS scheduler:
  * **High / Above Normal:** Main Thread, Render/RHI Submission (Hints the OS to aggressively use P-Cores).
  * **Critical:** Audio Thread (Mandatory to prevent audio popping/desync; interrupts all other workloads).
  * **Low / Below Normal:** Asset streaming, decompression, IO (Hints the OS to cleanly delegate work to E-Cores). 
As an engine scales, partitioning the `TaskScheduler` into separated "High-Priority Workers" and "Background Workers" becomes the optimal PC design pattern over hard-coded core masks.
