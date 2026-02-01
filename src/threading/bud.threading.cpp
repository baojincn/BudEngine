/// The Task Scheduler is the core component that powers the engine's Job System.

#include <atomic>
#include <vector>
#include <thread>
#include <optional>
#include <bit>
#include <functional>
#include <mutex>
#include <deque>
#include <print>
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

Fiber::Fiber(size_t stack_size) {
	stack_mem.resize(stack_size);
}

void Fiber::reset(std::move_only_function<void()>&& w, Counter* c, void (*entry_fn)(Fiber*)) {
	work = std::move(w);
	signal_counter = c;
	is_finished = false;
	next_waiting = nullptr;
	next_pool = nullptr;
	pending_wait_counter = nullptr;

#if defined(_DEBUG)
	debug_name = nullptr;
#endif

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



void LockFreeFiberPool::push(Fiber* f) {
	Fiber* old_head = head.load(std::memory_order_relaxed);

	do {
		f->next_pool = old_head;
	} while (!head.compare_exchange_weak(old_head, f, std::memory_order_release, std::memory_order_relaxed));
}


Fiber* LockFreeFiberPool::pop() {
	Fiber* old_head = head.load(std::memory_order_relaxed);

	do {
		if (!old_head) return nullptr;
	} while (!head.compare_exchange_weak(old_head, old_head->next_pool, std::memory_order_acquire, std::memory_order_relaxed));

	return old_head;
}



TaskScheduler::TaskScheduler(size_t n)
	: num_threads(n) {

	std::println("[TaskScheduler] Initializing with {} threads(workers), {} fibers per thread", n, MAX_FIBERS_PER_THREAD);

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
	for (auto w : workers) {
		delete w;
	}
	workers.clear();

	for (auto f : main_thread_incoming_queue)
		delete f;
	main_thread_incoming_queue.clear();

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
			std::unique_lock lock(main_queue_mtx, std::try_to_lock);
			if (lock.owns_lock() && !main_thread_incoming_queue.empty()) {
				f = main_thread_incoming_queue.front();
				main_thread_incoming_queue.pop_front();
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

	size_t idx = (t_scheduler) ? t_worker_index : 0u;
	workers[idx]->queue.push(f);
}

void TaskScheduler::spawn(std::move_only_function<void()> work, Counter* counter) {
	spawn(nullptr, std::move(work), counter);
}


void TaskScheduler::submit_main_thread_task(std::move_only_function<void()> work, Counter* counter) {
	auto f = allocate_fiber();
	f->reset(std::move(work), counter, &fiber_entry_stub);
	if (counter) counter->fetch_add(1, std::memory_order_relaxed);

	if (t_scheduler && t_worker_index == 0) {
		workers[0]->queue.push(f);
	}
	else {
		std::lock_guard lock(main_queue_mtx);
		main_thread_incoming_queue.push_back(f);
	}
}

void TaskScheduler::wait_for_counter(Counter& counter, std::function<void()> on_idle) {
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

			auto opt = workers[t_worker_index]->queue.pop();
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


void TaskScheduler::ParallelFor(size_t count, size_t chunk_size, std::function<void(size_t, size_t)> body, Counter* counter) {
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
		auto prev = self->signal_counter->value.fetch_sub(1, std::memory_order_acq_rel);
		if (prev == 1) {
			auto waiting_head = self->signal_counter->waiting_list.exchange(nullptr, std::memory_order_acquire);

			while (waiting_head) {
				auto next = waiting_head->next_waiting;
				waiting_head->next_waiting = nullptr;
				scheduler->workers[t_worker_index]->queue.push(waiting_head);
				waiting_head = next;
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

		if (c->value.load(std::memory_order_acquire) == 0) {
			auto wake_list = c->waiting_list.exchange(nullptr, std::memory_order_acquire);
			while (wake_list) {
				auto next = wake_list->next_waiting;
				wake_list->next_waiting = nullptr;
				workers[t_worker_index]->queue.push(wake_list);
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

	while (running && !st.stop_requested()) {
		Fiber* f = nullptr;

		if (index == 0) {
			std::unique_lock lock(main_queue_mtx, std::try_to_lock);
			if (lock.owns_lock() && !main_thread_incoming_queue.empty()) {
				f = main_thread_incoming_queue.front();
				main_thread_incoming_queue.pop_front();
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


