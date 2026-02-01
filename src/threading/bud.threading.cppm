/// The Task Scheduler is the core component that powers the engine's Job System.

module;
#include <atomic>
#include <vector>
#include <thread>
#include <optional>
#include <bit>
#include <functional>
#include <mutex>
#include <deque>



export module bud.threading;

export namespace bud::threading {
	struct Fiber;
	class TaskScheduler;

	/// <summary>
	/// A lightweight, thread-safe counter wrapper around std::atomic<int> that exposes basic atomic operations and holds an atomic pointer to a waiting list of Fiber objects.
	/// </summary>
	struct Counter {
		std::atomic<int> value{ 0 };
		std::atomic<Fiber*> waiting_list{ nullptr };

		Counter(int initial = 0);

		Counter(const Counter&) = delete;
		Counter& operator=(const Counter&) = delete;

		int fetch_add(int arg, std::memory_order order = std::memory_order_seq_cst);

		int fetch_sub(int arg, std::memory_order order = std::memory_order_seq_cst);

		int load(std::memory_order order = std::memory_order_seq_cst) const;

		void store(int arg, std::memory_order order = std::memory_order_seq_cst);
	};

	/// <summary>
	/// Fiber for lightweight cooperative multitasking in user space.
	/// </summary>
	struct alignas(16) Fiber {
		// Pool List
		Fiber* next_pool = nullptr;

		// Link to the next waiting fiber in the Counter's waiting list 
		Fiber* next_waiting = nullptr;

		void* rsp = nullptr;
		std::vector<uint8_t> stack_mem;
		std::move_only_function<void()> work;
		Counter* signal_counter = nullptr;

		bool is_finished = false;

		// Solved: "Double Run" Issue
		Counter* pending_wait_counter = nullptr;

		// Lightweight debug Info 
#if defined(_DEBUG)
		const char* debug_name = nullptr;
#endif

#if defined(_DEBUG) && defined(BUD_TRACK_TASK_SOURCE)
		std::stacktrace creation_stack;
#endif

#ifdef _DEBUG
		static constexpr size_t DEFAULT_STACK_SIZE = 64 * 1024; // 64KB
#else
		static constexpr size_t DEFAULT_STACK_SIZE = 32 * 1024; // 32KB
#endif

		Fiber(size_t stack_size = DEFAULT_STACK_SIZE);

		void reset(std::move_only_function<void()>&& w, Counter* c, void (*entry_fn)(Fiber*));
	};


	/*
	Top (队头)                               Bottom (队尾)
	[ Task 1 ]  [ Task 2 ]  ...  [ Task 99 ]  [ Task 100 ]
		^                                          ^
		|                                          |
	小偷 (Worker B)                            主人 (Worker A)
	偷走最老的 (Cold)                          处理最新的 (Hot)
	操作 Top 指针                              操作 Bottom 指针
	(FIFO)                                     (LIFO / Stack)
	*/

	/// <summary>
	/// A lock-free Work-Stealing Queue and pool
	/// </summary>
	constexpr size_t CACHE_LINE = 64;
	template<typename T>
	class alignas(CACHE_LINE) WorkStealingQueue {
		std::atomic<int64_t> top{ 0 };
		std::atomic<int64_t> bottom{ 0 };
		std::vector<T> buffer;
		size_t mask;
	public:
		explicit WorkStealingQueue(size_t capacity = 4096) {
			size_t cap = std::bit_ceil(capacity);
			buffer.resize(cap);
			mask = cap - 1;
		}

		void push(T item) {
			int64_t b = bottom.load(std::memory_order_relaxed);
			buffer[b & mask] = item;
			std::atomic_thread_fence(std::memory_order_release);
			bottom.store(b + 1, std::memory_order_relaxed);
		}

		std::optional<T> pop() {
			int64_t b = bottom.load(std::memory_order_relaxed) - 1;
			bottom.store(b, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			int64_t t = top.load(std::memory_order_relaxed);
			if (t <= b) {
				T item = buffer[b & mask];
				if (t == b) {
					if (!top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
						bottom.store(b + 1, std::memory_order_relaxed);
						return std::nullopt;
					}
					bottom.store(b + 1, std::memory_order_relaxed);
				}
				return item;
			}
			else {
				bottom.store(b + 1, std::memory_order_relaxed);
				return std::nullopt;
			}
		}

		std::optional<T> steal() {
			int64_t t = top.load(std::memory_order_acquire);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			int64_t b = bottom.load(std::memory_order_acquire);
			if (t < b) {
				T item = buffer[t & mask];
				if (!top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
					return std::nullopt;
				}
				return item;
			}
			return std::nullopt;
		}
	};


	class LockFreeFiberPool {
		std::atomic<Fiber*> head{ nullptr };
	public:
		void push(Fiber* f);

		Fiber* pop();
	};


	/// <summary>
	/// Thread-local pointer that stores a worker thread's stack pointer (RSP) for the current thread.
	/// </summary>
	thread_local void* t_worker_rsp = nullptr;
	thread_local Fiber* t_current_fiber = nullptr;
	thread_local size_t t_worker_index = 0;
	thread_local TaskScheduler* t_scheduler = nullptr;




	/// @brief High-performance Task Scheduler based on C++23 Fibers and Work Stealing.
	///
	/// -----------------------------------------------------------------------------
	/// @todo [Safety] Implement Stack Guard Pages.
	/// Current stack size is fixed (64KB Debug / 32KB Release). Deep recursion or 
	/// large stack allocations may silently corrupt heap memory (memory stomping).
	/// Action: Reserve a 4KB PAGE_NOACCESS page at the stack's low address to 
	/// trigger an immediate hardware exception on overflow.
	///
	/// @todo [Performance] Optimize Main Thread Queue.
	/// 'main_thread_incoming_queue_' currently relies on std::mutex.
	/// High contention may occur if multiple workers submit main-thread tasks simultaneously.
	/// Action: Replace with a lock-free MPSC (Multi-Producer Single-Consumer) queue.
	///
	/// @todo [Feature] Implement Task Priority System.
	/// Currently, all tasks are scheduled with equal priority. Latency-sensitive tasks
	/// (e.g., Audio, Physics) may get stuck behind long-running low-priority tasks.
	/// Action: Introduce High/Normal/Low priority queues, with Workers preferring High.
	///
	/// @todo [Portability] Cross-Platform Support.
	/// The context switch assembly (.asm) and stack alignment logic (top -= 40) 
	/// are hardcoded for the Windows x64 ABI.
	/// Action: Implement System V ABI (Linux/macOS) and ARM64 assembly paths 
	/// using #ifdef guards.
	///
	/// @warning [Usage] Strict Non-Blocking Policy.
	/// Fibers are cooperatively scheduled. Calling blocking APIs (e.g., Sleep, 
	/// fread, synchronous socket wait) will suspend the entire physical Worker Thread,
	/// starving all other fibers assigned to that thread.
	/// -----------------------------------------------------------------------------


	/// @brief The core Job System that manages fiber-based concurrency.
	///
	/// This scheduler supports three primary patterns of execution (Task Types),
	/// allowing for both logic-driven and data-driven parallelism.
	///
	/// -----------------------------------------------------------------------------
	/// **1. Fire-and-Forget Tasks (Detached)**
	/// Independent background tasks that do not require immediate synchronization.
	/// Suitable for: Logging, file I/O requests, or non-critical background logic.
	/// @code
	/// scheduler.spawn([]() {
	///     Log::Write("Background task started.");
	/// });
	/// @endcode
	///
	/// -----------------------------------------------------------------------------
	/// **2. Dependency-Managed Tasks (Fork-Join / DAG)**
	/// Tasks that define an implicit dependency graph using `Counter`.
	/// The current thread (Main or Worker) can spawn children and wait for them 
	/// without blocking the physical thread (it will steal work while waiting).
	/// Suitable for: Game loop stages (e.g., Physics -> wait -> Rendering).
	/// @code
	/// Counter dependency;
	/// scheduler.spawn(TaskA, &dependency); // Fork
	/// scheduler.spawn(TaskB, &dependency); // Fork
	/// scheduler.wait_for_counter(dependency); // Join (Sync point)
	/// @endcode
	///
	/// -----------------------------------------------------------------------------
	/// **3. Data Parallelism (Batch Processing)**
	/// High-throughput processing of large datasets (Arrays, Vectors).
	/// Large loops are split into chunks and distributed across worker threads.
	/// Suitable for: Particle updates, Frustum Culling, Animation blending.
	/// @code
	/// // Example conceptual usage (via a ParallelFor helper):
	/// scheduler.ParallelFor(10000, 500, [](size_t start, size_t end) {
	///     for (size_t i = start; i < end; ++i) UpdateParticle(i);
	/// }, &dependency);
	/// @endcode
	/// -----------------------------------------------------------------------------


	export class TaskScheduler {
		struct alignas(CACHE_LINE) Worker {
			std::jthread thread;
			WorkStealingQueue<Fiber*> queue;
		};

		std::vector<Worker*> workers;
		LockFreeFiberPool fiber_pool;
		std::atomic<bool> running{ true };
		size_t num_threads{ 0 };

		// Only for main thread tasks
		std::deque<Fiber*> main_thread_incoming_queue;
		std::mutex main_queue_mtx;

		static constexpr size_t MAX_FIBERS_PER_THREAD = 128;

	public:
		explicit TaskScheduler(size_t n = std::thread::hardware_concurrency());

		/// <summary>
		/// /// @brief Destructor.
		/// 
		/// @note DESTRUCTION ORDER IS CRITICAL:
		/// 1. stop(): Tell threads to exit their loops.
		/// 2. delete workers: Triggers std::jthread::join(). This forces the main thread
		///    to wait until all background threads have completely finished.
		///    We MUST do this before deleting fibers, otherwise background threads
		///    might try to access a deleted fiber pool (Use-After-Free).
		/// 3. delete fibers: Now that all threads are dead, it is safe to clean up memory.
		/// </summary>
		~TaskScheduler();

		void stop();

		size_t get_thread_count() const;

		/// <summary>
		/// Initialize the main thread as a worker (index 0).
		/// </summary>
		void init_main_thread_worker();


		/// <summary>
		/// Pump and execute tasks that are must scheduled to run on the main thread.
		/// Then, also process tasks in the main worker's queue.
		/// </summary>
		void pump_main_thread_tasks();


		void spawn(const char* name, std::move_only_function<void()> work, Counter* counter = nullptr);

		void spawn(std::move_only_function<void()> work, Counter* counter = nullptr);

		/// <summary>
		/// Submit a task that must be executed on the main thread.
		/// </summary>
		void submit_main_thread_task(std::move_only_function<void()> work, Counter* counter = nullptr);

		void wait_for_counter(Counter& counter, std::function<void()> on_idle = nullptr);

		/// <summary>
		/// Runs a parallel loop over count iterations by dividing the work into chunks of up to chunk_size
		/// and spawning a task for each chunk. For each index j in [0, count), the provided body callable is invoked.
		/// </summary>

		void ParallelFor(size_t count, size_t chunk_size, std::function<void(size_t, size_t)> body, Counter* counter);

	private:
		Fiber* allocate_fiber();

		void free_fiber(Fiber* f);

		/// <summary>
		/// Entry routine for a fiber: runs the fiber's work callback, resolves dependency waiters when the fiber signals completion, marks the fiber finished, and switches context back to the worker.
		/// </summary>
		/// <param name="f_dummy">An unused placeholder parameter to match the fiber entry signature. The function instead operates on the thread-local current fiber (t_current_fiber) and scheduler (t_scheduler).</param>
		static void fiber_entry_stub(Fiber* f_dummy);


		void execute_task(Fiber* f);

		Fiber* steal_task(size_t my_idx);

		void worker_loop(size_t index, std::stop_token st);
	};
}
