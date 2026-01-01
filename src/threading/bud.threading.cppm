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
#include <print>
#include <stacktrace>

#ifdef TRACY_ENABLE
	#include <tracy/Tracy.hpp>
	#include <stdio.h>
#else
	// Define empty macros to avoid Tracy calls when disabled
	#define ZoneScoped
	#define ZoneScopedN(x)
	#define FrameMark
#endif

// Assembly hooks
#ifdef _WIN64
extern "C" void bud_switch_context_win64(void** old_rsp, void* new_rsp);
#define bud_switch_context bud_switch_context_win64
#elif defined(__linux__) && defined(__x86_64__)
extern "C" void bud_switch_context_linux(void** old_rsp, void* new_rsp);
#define bud_switch_context bud_switch_context_linux
#endif

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

        Counter(int initial = 0) : value(initial) {}

        Counter(const Counter&) = delete;
        Counter& operator=(const Counter&) = delete;

		int fetch_add(int arg, std::memory_order order = std::memory_order_seq_cst) {
			return value.fetch_add(arg, order);
		}

		int fetch_sub(int arg, std::memory_order order = std::memory_order_seq_cst) {
			return value.fetch_sub(arg, order);
		}

		int load(std::memory_order order = std::memory_order_seq_cst) const {
			return value.load(order);
		}

		void store(int arg, std::memory_order order = std::memory_order_seq_cst) {
			value.store(arg, order);
		}
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

#ifdef _DEBUG
		std::stacktrace creation_stack;
#endif

#ifdef _DEBUG
		static constexpr size_t DEFAULT_STACK_SIZE = 64 * 1024; // 64KB
#else
		static constexpr size_t DEFAULT_STACK_SIZE = 32 * 1024; // 32KB
#endif

        Fiber(size_t stack_size = DEFAULT_STACK_SIZE) {
			stack_mem.resize(stack_size);
		}

		void reset(std::move_only_function<void()>&& w, Counter* c, void (*entry_fn)(Fiber*)) {
			work = std::move(w);
			signal_counter = c;
			is_finished = false;
			next_waiting = nullptr;
			next_pool = nullptr;
			pending_wait_counter = nullptr;

			// Initialize the stack pointer (RSP) for the fiber
			uintptr_t top = reinterpret_cast<uintptr_t>(stack_mem.data() + stack_mem.size());


			/// @note [Windows x64 ABI Compliance]
			/// We subtract 40 bytes here to ensure the stack is correctly aligned for the
			/// 'entry_fn' function call.
			/// 
			/// 1. Windows x64 requires that before a 'CALL' instruction, the RSP must be
			///    16-byte aligned. After the 'CALL' pushes the 8-byte return address,
			///    the RSP at the function entry point must end in 8 (e.g., ...08, ...18).
			/// 
			/// 2. Calculation:
			///    - Start: 16-byte aligned address (top &= ~0xF).
			///    - Shadow Space: 32 bytes (required by x64 ABI for callees).
			///    - Alignment Pad: 8 bytes (to offset the RSP so it ends in 8).
			///    - Total: 32 + 8 = 40 bytes.
			/// 
			/// This ensures that SIMD instructions (like 'movaps') inside the function
			/// won't crash due to misalignment.
			top &= ~0xF;
#ifdef _WIN64
			top -= 40;
#elifdef __linux__ && __x86_64__
			// Linux x86_64 System V ABI
			// Similar alignment considerations apply, but no shadow space is needed.
			top -= 8; // Just align for return address
#endif

			auto ptr = reinterpret_cast<void**>(top);

			// For ret
			*(--ptr) = reinterpret_cast<void*>(entry_fn);

			// For 8 registers (RBP, RBX, R12-R15, RDI, RSI)
			for (int i = 0; i < 8; ++i)
				*(--ptr) = nullptr;

			// Reserve 160 bytes for XMM registers (XMM6-XMM15)
			auto byte_ptr = reinterpret_cast<uint8_t*>(ptr);
			byte_ptr -= 160;
			rsp = static_cast<void*>(byte_ptr);
		}
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
        std::atomic<int64_t> top_{ 0 };
        std::atomic<int64_t> bottom_{ 0 };
        std::vector<T> buffer_;
        size_t mask_;
    public:
        explicit WorkStealingQueue(size_t capacity = 4096) {
            size_t cap = std::bit_ceil(capacity);
            buffer_.resize(cap);
            mask_ = cap - 1;
        }

        void push(T item) {
            int64_t b = bottom_.load(std::memory_order_relaxed);
            buffer_[b & mask_] = item;
            std::atomic_thread_fence(std::memory_order_release);
            bottom_.store(b + 1, std::memory_order_relaxed);
        }

        std::optional<T> pop() {
            int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
            bottom_.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t t = top_.load(std::memory_order_relaxed);
            if (t <= b) {
                T item = buffer_[b & mask_];
                if (t == b) {
                    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                        bottom_.store(b + 1, std::memory_order_relaxed);
                        return std::nullopt;
                    }
                    bottom_.store(b + 1, std::memory_order_relaxed);
                }
                return item;
            }
            else {
                bottom_.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }
        }

        std::optional<T> steal() {
            int64_t t = top_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t b = bottom_.load(std::memory_order_acquire);
            if (t < b) {
                T item = buffer_[t & mask_];
                if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    return std::nullopt;
                }
                return item;
            }
            return std::nullopt;
        }
    };

    class LockFreeFiberPool {
        std::atomic<Fiber*> head_{ nullptr };
    public:
        void push(Fiber* f) {
            Fiber* old_head = head_.load(std::memory_order_relaxed);
            do {
				f->next_pool = old_head;
			}
			while (!head_.compare_exchange_weak(old_head, f, std::memory_order_release, std::memory_order_relaxed));
        }
        Fiber* pop() {
            Fiber* old_head = head_.load(std::memory_order_relaxed);
            do {
				if (!old_head) return nullptr;
			}
			while (!head_.compare_exchange_weak(old_head, old_head->next_pool, std::memory_order_acquire, std::memory_order_relaxed));
            return old_head;
        }
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

        std::vector<Worker*> workers_;
        LockFreeFiberPool fiber_pool_;
        std::atomic<bool> running_{ true };
        size_t num_threads_;

		// Only for main thread tasks
        std::deque<Fiber*> main_thread_incoming_queue_;
        std::mutex main_queue_mtx_;

		static constexpr size_t MAX_FIBERS_PER_THREAD = 128;

    public:
        explicit TaskScheduler(size_t n = std::thread::hardware_concurrency())
            : num_threads_(n) {

			std::println("[TaskScheduler] Initializing with {} threads(workers), {} fibers per thread", n, MAX_FIBERS_PER_THREAD);

            for (size_t i = 0; i < n * MAX_FIBERS_PER_THREAD; ++i)
				fiber_pool_.push(new Fiber());

            workers_.reserve(n);

			for (size_t i = 0; i < n; ++i) {
				workers_.push_back(new Worker());
			}

			for (size_t i = 1; i < n; ++i) {
				workers_[i]->thread = std::jthread([this, i](std::stop_token st) {
					worker_loop(i, st);
				});
			}
        }

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
        ~TaskScheduler() {
			stop();
			for (auto w : workers_) {
				delete w;
			}
			workers_.clear();

			for (auto f : main_thread_incoming_queue_)
				delete f;
			main_thread_incoming_queue_.clear();

			Fiber* f = nullptr;
			while ((f = fiber_pool_.pop()) != nullptr) {
				delete f;
			}
		}

        void stop() {
			running_ = false;
		}

		/// <summary>
		/// Initialize the main thread as a worker (index 0).
		/// </summary>
		void init_main_thread_worker() {
			t_worker_index = 0;
			t_scheduler = this;
		}


		/// <summary>
		/// Pump and execute tasks that are must scheduled to run on the main thread.
		/// Then, also process tasks in the main worker's queue.
		/// </summary>
		void pump_main_thread_tasks() {
			while (true) {
				Fiber* f = nullptr;
				{
					std::unique_lock lock(main_queue_mtx_, std::try_to_lock);
					if (lock.owns_lock() && !main_thread_incoming_queue_.empty()) {
						f = main_thread_incoming_queue_.front();
						main_thread_incoming_queue_.pop_front();
					}
				}

				if (f)
					execute_task(f);
				else
					break; // No more tasks
			}

			while (auto opt = workers_[0]->queue.pop()) {
				execute_task(*opt);
			}
		}


        void spawn(std::move_only_function<void()> work, Counter* counter = nullptr) {
            auto f = allocate_fiber();

#ifdef _DEBUG
			f->creation_stack = std::stacktrace::current();
#endif

            f->reset(std::move(work), counter, &fiber_entry_stub);
            if (counter)
				counter->fetch_add(1, std::memory_order_relaxed);

            size_t idx = (t_scheduler) ? t_worker_index : 0u;
            workers_[idx]->queue.push(f);
        }

		/// <summary>
		/// Submit a task that must be executed on the main thread.
		/// </summary>
        void submit_main_thread_task(std::move_only_function<void()> work, Counter* counter = nullptr) {
            auto f = allocate_fiber();
            f->reset(std::move(work), counter, &fiber_entry_stub);
            if (counter) counter->fetch_add(1, std::memory_order_relaxed);

            if (t_scheduler && t_worker_index == 0) {
                workers_[0]->queue.push(f);
            }
            else {
                std::lock_guard lock(main_queue_mtx_);
                main_thread_incoming_queue_.push_back(f);
            }
        }

		void wait_for_counter(Counter& counter, std::function<void()> on_idle = nullptr) {
			if (counter.value.load(std::memory_order_acquire) == 0)
				return;

			if (t_current_fiber) {
				t_current_fiber->pending_wait_counter = &counter; // Only flag here
				bud_switch_context(&t_current_fiber->rsp, t_worker_rsp);
			}
			else {
				while (counter.value.load(std::memory_order_acquire) > 0) {
					if (on_idle)
						on_idle();

					Fiber* f = nullptr;

					auto opt = workers_[t_worker_index]->queue.pop();
					if (opt)
						f = *opt;

					if (!f)
						f = steal_task(t_worker_index);

					if (f) {
						execute_task(f);
					}
					else {
						std::this_thread::yield();
					}
				}
			}
		}

		/// <summary>
		/// Runs a parallel loop over count iterations by dividing the work into chunks of up to chunk_size
		/// and spawning a task for each chunk. For each index j in [0, count), the provided body callable is invoked.
		/// </summary>

		void ParallelFor(size_t count, size_t chunk_size, std::function<void(size_t, size_t)> body, Counter* counter) {
			auto batch_count = (count + chunk_size - 1) / chunk_size;

			for (size_t i = 0; i < batch_count; ++i) {
				auto start = i * chunk_size;
				auto end = std::min(start + chunk_size, count);

				spawn([start, end, body]() {
					for (auto j = start; j < end; ++j) {
						body(j, j);
					}
				}, counter);
			}
		}

    private:
        Fiber* allocate_fiber() {
            auto f = fiber_pool_.pop();
            return f ? f : new Fiber();
        }

        void free_fiber(Fiber* f) {
            fiber_pool_.push(f);
        }

        /// <summary>
        /// Entry routine for a fiber: runs the fiber's work callback, resolves dependency waiters when the fiber signals completion, marks the fiber finished, and switches context back to the worker.
        /// </summary>
        /// <param name="f_dummy">An unused placeholder parameter to match the fiber entry signature. The function instead operates on the thread-local current fiber (t_current_fiber) and scheduler (t_scheduler).</param>
        static void fiber_entry_stub(Fiber* f_dummy) {
            auto self = t_current_fiber;
            auto scheduler = t_scheduler;

            if (self->work)
				self->work();					

			// Process dependencies
            if (self->signal_counter) {
                auto prev = self->signal_counter->value.fetch_sub(1, std::memory_order_acq_rel);
                if (prev == 1) {
                    auto waiting_head = self->signal_counter->waiting_list.exchange(nullptr, std::memory_order_acquire);

                    while (waiting_head) {
                        auto next = waiting_head->next_waiting;
                        waiting_head->next_waiting = nullptr;
                        scheduler->workers_[t_worker_index]->queue.push(waiting_head);
                        waiting_head = next;
                    }
                }
            }

            self->is_finished = true;
            bud_switch_context(&self->rsp, t_worker_rsp);
        }


        void execute_task(Fiber* f) {
			ZoneScoped;

			t_current_fiber = f;
			bud_switch_context(&t_worker_rsp, f->rsp);

			t_current_fiber = nullptr;

			// Post-Switch Wait
			if (f->pending_wait_counter) {
				auto c = f->pending_wait_counter;
				f->pending_wait_counter = nullptr;

				auto old_head = c->waiting_list.load(std::memory_order_relaxed);

				do {
					f->next_waiting = old_head;
				}
				while (!c->waiting_list.compare_exchange_weak(
					old_head, f,
					std::memory_order_release, std::memory_order_relaxed));

				if (c->value.load(std::memory_order_acquire) == 0) {
					auto wake_list = c->waiting_list.exchange(nullptr, std::memory_order_acquire);
					while (wake_list) {
						auto next = wake_list->next_waiting;
						wake_list->next_waiting = nullptr;
						workers_[t_worker_index]->queue.push(wake_list);
						wake_list = next;
					}
				}

				return;
			}

            if (f->is_finished) {
                f->work = nullptr;
                free_fiber(f);
            }
        }

        Fiber* steal_task(size_t my_idx) {
            for (size_t i = 1; i < num_threads_; ++i) {
                size_t victim = (my_idx + i) % num_threads_;
                auto opt = workers_[victim]->queue.steal();
                if (opt)
					return *opt;
            }
            return nullptr;
        }

		void worker_loop(size_t index, std::stop_token st) {
			t_worker_index = index;
			t_scheduler = this;

#ifdef TRACY_ENABLE
			char thread_name[32];
			if (index == 0)
				std::snprintf(thread_name, sizeof(thread_name), "Main Worker");
			else
				std::snprintf(thread_name, sizeof(thread_name), "Worker %zu", index);
			tracy::SetThreadName(thread_name);
#endif

			while (running_ && !st.stop_requested()) {
				Fiber* f = nullptr;

                if (index == 0) {
                    std::unique_lock lock(main_queue_mtx_, std::try_to_lock);
                    if (lock.owns_lock() && !main_thread_incoming_queue_.empty()) {
                        f = main_thread_incoming_queue_.front();
                        main_thread_incoming_queue_.pop_front();
                    }
                }

                if (!f) {
                    auto opt = workers_[index]->queue.pop();
                    if (opt)
						f = *opt;
                }

                if (!f)
					f = steal_task(index);

                if (f) {
                    execute_task(f);
                }
                else {
					std::this_thread::sleep_for(std::chrono::microseconds(1));
                }
            }
        }
    };
}
