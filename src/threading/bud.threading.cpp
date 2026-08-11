/// The Task Scheduler is the core component that powers the engine's Job System.

#include <atomic>
#include <vector>
#include <thread>
#include <optional>
#include <bit>
#include <functional>
#include <mutex>
#include <deque>
#include <new>
#include <print>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "src/core/bud.core.hpp"
// #define BUD_TRACK_TASK_SOURCE
#if defined(_DEBUG) && defined(BUD_TRACK_TASK_SOURCE)
#include <stacktrace>
#endif

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#include <stdio.h>
#else
	// Define empty macros to avoid Tracy calls when disabled
#define ZoneScoped
#define ZoneScopedN(x)
#define FrameMark
#endif

#include "src/threading/bud.threading.hpp"

// Assembly hooks
#ifdef _WIN64
extern "C" void bud_switch_context_win64(void** old_rsp, void* new_rsp);
#define bud_switch_context bud_switch_context_win64
#elif defined(__linux__) && defined(__x86_64__)
extern "C" void bud_switch_context_linux(void** old_rsp, void* new_rsp);
#define bud_switch_context bud_switch_context_linux
#endif


using namespace bud::threading;

namespace {
	// Clamp a raw worker index (-1 = "not a worker") to a valid worker slot.
	// Casting -1 to size_t directly would index the workers vector out of
	// bounds and corrupt the heap, so guard every call site.
	inline size_t clamp_worker_index(int raw, size_t worker_count) {
		if (raw < 0 || worker_count == 0)
			return 0;
		return static_cast<size_t>(raw) % worker_count;
	}
}

Counter::Counter(int initial) : value(initial) {}

int Counter::fetch_add(int arg, std::memory_order order) {
	return value.fetch_add(arg, order);
}

int Counter::fetch_sub(int arg, std::memory_order order) {
	return value.fetch_sub(arg, order);
}

int Counter::load(std::memory_order order) const {
	return value.load(order);
}

void Counter::store(int arg, std::memory_order order) {
	value.store(arg, order);
}

Fiber::Fiber(size_t stack_size) : stack_size(stack_size) {
#ifdef _WIN32
	// Allocate [guard page][usable stack]. The fiber stack grows downward, so
	// the guard page is placed at the LOW end of the region: any stack overflow
	// hits PAGE_NOACCESS and faults immediately instead of corrupting the heap.
	constexpr size_t page_size = 4096;
	const size_t total = stack_size + page_size;
	uint8_t* region = static_cast<uint8_t*>(
		VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (!region)
		throw std::bad_alloc();

	DWORD old_protect = 0;
	if (!VirtualProtect(region, page_size, PAGE_NOACCESS, &old_protect)) {
		VirtualFree(region, 0, MEM_RELEASE);
		throw std::bad_alloc();
	}

	stack_region = region;
	stack_base = region + page_size;
#else
	// Non-Windows fallback: plain heap allocation (no guard page).
	stack_base = ::operator new(stack_size);
	stack_region = nullptr;
#endif
}

Fiber::~Fiber() {
#ifdef _WIN32
	if (stack_region)
		VirtualFree(stack_region, 0, MEM_RELEASE);
#else
	if (stack_base)
		::operator delete(stack_base);
#endif
	stack_region = nullptr;
	stack_base = nullptr;
}

void Fiber::reset(std::move_only_function<void()>&& w, Counter* c, void (*entry_fn)(Fiber*)) {
	work = std::move(w);
	signal_counter = c;
	is_finished = false;
	next_waiting = nullptr;
	next_pool.store(nullptr, std::memory_order_relaxed);
	pending_wait_counter = nullptr;
	target_thread_index = -1;

#if defined(_DEBUG)
	debug_name = nullptr;
#endif

	// Initialize the stack pointer (RSP) for the fiber
	uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;


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



void LockFreeFiberPool::push(Fiber* f) {
	uintptr_t old_head = head.load(std::memory_order_relaxed);

	do {
		f->next_pool.store(reinterpret_cast<Fiber*>(old_head & kPointerMask), std::memory_order_relaxed);
	} while (!head.compare_exchange_weak(old_head,
		(old_head & kTagMask) | (reinterpret_cast<uintptr_t>(f) & kPointerMask),
		std::memory_order_release, std::memory_order_relaxed));
}


Fiber* LockFreeFiberPool::pop() {
	uintptr_t old_head = head.load(std::memory_order_relaxed);

	do {
		Fiber* result = reinterpret_cast<Fiber*>(old_head & kPointerMask);
		if (!result) return nullptr;

		Fiber* next = result->next_pool.load(std::memory_order_relaxed);
		uintptr_t new_head = ((old_head + kTagStep) & kTagMask) | (reinterpret_cast<uintptr_t>(next) & kPointerMask);

		if (head.compare_exchange_weak(old_head, new_head, std::memory_order_acquire, std::memory_order_relaxed))
			return result;
	} while (true);
}

TaskScheduler::TaskScheduler(size_t n)
	: num_threads(n) {

	bud::print("[TaskScheduler] Initializing with {} threads(workers), {} fibers per thread", n, MAX_FIBERS_PER_THREAD);

	for (size_t i = 0; i < n * MAX_FIBERS_PER_THREAD; ++i)
		fiber_pool.push(new Fiber());

	workers.reserve(n);

	for (size_t i = 0; i < n; ++i) {
		workers.push_back(new Worker());
	}

	for (size_t i = 1; i < n; ++i) {
		workers[i]->thread = std::jthread([this, i](std::stop_token st) {
			worker_loop(i, st);
		});
	}
}

TaskScheduler::~TaskScheduler() {
    stop();

    // Ensure background threads have exited before we delete any shared data
    // Join any std::jthread owned by workers first. This prevents worker
    // threads from accessing the pinned_queue or the fiber_pool while we
    // are destroying them (use-after-free).
    for (size_t i = 0; i < workers.size(); ++i) {
        if (workers[i]) {
            auto &jt = workers[i]->thread;
            if (jt.joinable()) {
                // Request stop first to signal worker loop to exit.
                // Avoid joining the calling thread (would throw) by
                // comparing thread ids; if it's the same thread, only
                // request stop and skip join.
                if (jt.get_id() != std::this_thread::get_id()) {
                    jt.request_stop();
                    jt.join();
                } else {
                    jt.request_stop();
                }
            }
        }
    }

    // Now it is safe to destroy pinned queues and worker objects. Return any
    // fibers still parked in worker queues back into the pool so the final drain
    // below deletes every node exactly once (no leak, no double-free).
    for (auto w : workers) {
        while (auto opt = w->queue.pop())
            fiber_pool.push(*opt);
        {
            std::lock_guard lock(w->pinned_mtx);
            for (auto f : w->pinned_queue)
                fiber_pool.push(f);
            w->pinned_queue.clear();
        }
        delete w;
    }
    workers.clear();

    // Finally, delete any remaining fibers in the pool
    Fiber* f = nullptr;
    while ((f = fiber_pool.pop()) != nullptr) {
        delete f;
    }
}

void TaskScheduler::stop() {
	running = false;
}

size_t TaskScheduler::get_thread_count() const {
	return num_threads;
}


void TaskScheduler::init_main_thread_worker() {
	t_worker_index = 0;
	t_scheduler = this;
}


void TaskScheduler::pump_main_thread_tasks() {
	while (true) {
		Fiber* f = nullptr;
		{
			std::unique_lock lock(workers[0]->pinned_mtx, std::try_to_lock);
			if (lock.owns_lock() && !workers[0]->pinned_queue.empty()) {
				f = workers[0]->pinned_queue.front();
				workers[0]->pinned_queue.pop_front();
			}
		}

		if (f)
			execute_task(f);
		else
			break; // No more tasks
	}

	while (auto opt = workers[0]->queue.pop()) {
		execute_task(*opt);
	}
}


void TaskScheduler::spawn(const char* name, std::move_only_function<void()> work, Counter* counter) {
	auto f = allocate_fiber();

#if defined(_DEBUG)
	f->debug_name = name;
#endif

#if defined(_DEBUG) && defined(BUD_TRACK_TASK_SOURCE)
	f->creation_stack = std::stacktrace::current();
#endif

	f->reset(std::move(work), counter, &fiber_entry_stub);
	if (counter)
		counter->fetch_add(1, std::memory_order_relaxed);

    size_t idx = clamp_worker_index(t_scheduler ? t_worker_index : 0, workers.size());
    workers[idx]->queue.push(f);
}

void TaskScheduler::spawn(std::move_only_function<void()> work, Counter* counter) {
	spawn(nullptr, std::move(work), counter);
}


void TaskScheduler::spawn_on_thread(uint32_t thread_index, const char* name, std::move_only_function<void()> work, Counter* counter) {
	if (thread_index >= num_threads) thread_index = 0;
	auto f = allocate_fiber();

#if defined(_DEBUG)
	f->debug_name = name;
#endif

#if defined(_DEBUG) && defined(BUD_TRACK_TASK_SOURCE)
	f->creation_stack = std::stacktrace::current();
#endif

	f->reset(std::move(work), counter, &fiber_entry_stub);
	f->target_thread_index = static_cast<int>(thread_index);
	if (counter)
		counter->fetch_add(1, std::memory_order_relaxed);

	std::lock_guard lock(workers[thread_index]->pinned_mtx);
	workers[thread_index]->pinned_queue.push_back(f);
}

void TaskScheduler::spawn_on_thread(uint32_t thread_index, std::move_only_function<void()> work, Counter* counter) {
	spawn_on_thread(thread_index, nullptr, std::move(work), counter);
}


void TaskScheduler::submit_main_thread_task(std::move_only_function<void()> work, Counter* counter) {
	spawn_on_thread(0, nullptr, std::move(work), counter);
}

void TaskScheduler::wait_for_counter(Counter& counter, std::function<void()> on_idle) {
	if (t_current_fiber) {
		// Fiber path: park on the counter's waiting list while value > 0. When
		// the final task detaches the list it wakes us; the value is either
		// still -1 (draining) or already 0, both handled by the loop below.
		while (counter.value.load(std::memory_order_acquire) != 0) {
			int v = counter.value.load(std::memory_order_acquire);
			if (v > 0) {
				t_current_fiber->pending_wait_counter = &counter; // Only flag here
				bud_switch_context(&t_current_fiber->rsp, t_worker_rsp);
			}
			else {
				// v == -1: the final task is still draining (waking parked
				// fibers); it will publish 0 right after. Spin so we do not
				// destroy this Counter (on our stack) while it is in use.
				std::this_thread::yield();
			}
		}
		return;
	}

	// Main-thread path: run tasks until the counter is satisfied.
	const size_t my_idx = clamp_worker_index(t_worker_index, workers.size());
	while (counter.value.load(std::memory_order_acquire) > 0) {
		if (on_idle)
			on_idle();

		Fiber* f = nullptr;

		auto opt = workers[my_idx]->queue.pop();
		if (opt)
			f = *opt;

		if (!f)
			f = steal_task(my_idx);

		if (f) {
			execute_task(f);
		}
		else {
			std::this_thread::yield();
		}
	}

	// value is now 0 (finished) or -1 (the final task is still waking parked
	// fibers). Wait for the drain to publish 0 before returning so this Counter
	// (typically a stack object owned by this caller) is not destroyed while
	// the final task still references it.
	while (counter.value.load(std::memory_order_acquire) != 0)
		std::this_thread::yield();
}


void TaskScheduler::ParallelFor(size_t count, size_t chunk_size, std::function<void(size_t, size_t)> body, Counter* counter) {
	auto batch_count = (count + chunk_size - 1) / chunk_size;

	for (size_t i = 0; i < batch_count; ++i) {
		auto start = i * chunk_size;
		auto end = std::min(start + chunk_size, count);

		spawn([start, end, body]() {
			body(start, end);
		}, counter);
	}
}


Fiber* TaskScheduler::allocate_fiber() {
	auto f = fiber_pool.pop();
	return f ? f : new Fiber();
}


void TaskScheduler::free_fiber(Fiber* f) {
	fiber_pool.push(f);
}


void TaskScheduler::fiber_entry_stub(Fiber* f_dummy) {
	auto self = t_current_fiber;
	auto scheduler = t_scheduler;

	if (self->work)
		self->work();

	// Process dependencies
	if (self->signal_counter) {
		auto& c = *self->signal_counter;
		// Decrement the task counter with a CAS protocol that gives the final
		// task a "draining" window (value == -1) between detaching the waiting
		// list and publishing 0. wait_for_counter treats -1 as "still draining",
		// so a caller never destroys the Counter while the final task still
		// references it (fixes a use-after-free of stack Counter objects).
		int v = c.value.load(std::memory_order_acquire);
		while (v > 0) {
			if (v == 1) {
				int expected = 1;
				if (c.value.compare_exchange_weak(expected, -1, std::memory_order_acq_rel, std::memory_order_acquire)) {
					// Last task: wake every parked fiber, then publish 0.
					auto waiting_head = c.waiting_list.exchange(nullptr, std::memory_order_acquire);
					while (waiting_head) {
						auto next = waiting_head->next_waiting;
						waiting_head->next_waiting = nullptr;
						if (waiting_head->target_thread_index != -1) {
							auto tidx = clamp_worker_index(waiting_head->target_thread_index, scheduler->workers.size());
							std::lock_guard lock(scheduler->workers[tidx]->pinned_mtx);
							scheduler->workers[tidx]->pinned_queue.push_back(waiting_head);
						} else {
							auto idx = clamp_worker_index(t_worker_index, scheduler->workers.size());
							scheduler->workers[idx]->queue.push(waiting_head);
						}
						waiting_head = next;
					}
					c.value.store(0, std::memory_order_release);
					break;
				}
				v = expected; // CAS lost; retry with the fresh value
			}
			else {
				if (c.value.compare_exchange_weak(v, v - 1, std::memory_order_acq_rel, std::memory_order_acquire))
					break; // not the last task
				// else retry with the updated v
			}
		}
	}

	self->is_finished = true;
	bud_switch_context(&self->rsp, t_worker_rsp);
}


void TaskScheduler::execute_task(Fiber* f) {
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
		} while (!c->waiting_list.compare_exchange_weak(
			old_head, f,
			std::memory_order_release, std::memory_order_relaxed));

		// value <= 0 covers both "finished" (0) and "final task still draining"
		// (-1). In the -1 case the final task also drains via exchange; the two
		// races resolve atomically, and if we inserted after its exchange we must
		// wake ourselves here or we would be stranded on the list.
		if (c->value.load(std::memory_order_acquire) <= 0) {
			auto wake_list = c->waiting_list.exchange(nullptr, std::memory_order_acquire);
			while (wake_list) {
				auto next = wake_list->next_waiting;
				wake_list->next_waiting = nullptr;
				if (wake_list->target_thread_index != -1) {
					auto tidx = clamp_worker_index(wake_list->target_thread_index, workers.size());
					std::lock_guard lock(workers[tidx]->pinned_mtx);
					workers[tidx]->pinned_queue.push_back(wake_list);
				} else {
					auto idx = clamp_worker_index(t_worker_index, workers.size());
					workers[idx]->queue.push(wake_list);
				}
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


Fiber* TaskScheduler::steal_task(size_t my_idx) {
	for (size_t i = 1; i < num_threads; ++i) {
		size_t victim = (my_idx + i) % num_threads;
		auto opt = workers[victim]->queue.steal();
		if (opt)
			return *opt;
	}
	return nullptr;
}


void TaskScheduler::worker_loop(size_t index, std::stop_token st) {
    t_worker_index = static_cast<int>(index);
	t_scheduler = this;

#ifdef TRACY_ENABLE
	char thread_name[32];
	if (index == 0)
		std::snprintf(thread_name, sizeof(thread_name), "Main Worker");
	else
		std::snprintf(thread_name, sizeof(thread_name), "Worker %zu", index);
	tracy::SetThreadName(thread_name);
#endif

	while (running && !st.stop_requested()) {
		Fiber* f = nullptr;

		{
			std::unique_lock lock(workers[index]->pinned_mtx, std::try_to_lock);
			if (lock.owns_lock() && !workers[index]->pinned_queue.empty()) {
				f = workers[index]->pinned_queue.front();
				workers[index]->pinned_queue.pop_front();
			}
		}

		if (!f) {
			auto opt = workers[index]->queue.pop();
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


