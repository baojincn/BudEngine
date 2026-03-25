#include "bud.logger.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <print>
#include <filesystem>
#include "src/threading/bud.threading.hpp"
#include "src/io/bud.io.hpp"
#include <map>
#include <optional>

namespace bud {

	std::string make_log_prefix() {
		using namespace std::chrono;
		auto now = system_clock::now();
		auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
		std::time_t t = system_clock::to_time_t(now);
		std::tm tm_info{};
#ifdef _WIN32
		localtime_s(&tm_info, &t);
#else
		localtime_r(&t, &tm_info);
#endif

		std::ostringstream ss;
		ss << std::put_time(&tm_info, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count();
		// thread id as hashed value for compactness
		auto tid = std::this_thread::get_id();
		size_t tid_hash = std::hash<std::thread::id>{}(tid);
		ss << " [TID=" << tid_hash << "] ";
		return ss.str();
	}

	// Thread-safe global non-owning pointer accessor using std::atomic.
	static std::atomic<Logger*>& global_logger_ref() {
		static std::atomic<Logger*> ptr{ nullptr };
		return ptr;
	}

	// Sequence reservation used for ordered logging
	static std::atomic<uint64_t> g_next_log_sequence{ 0 };

	void set_global_logger(Logger* logger) {
		global_logger_ref().store(logger, std::memory_order_release);
	}

	// Enqueue ordered logs implementation
	void Logger::enqueue_ordered(uint64_t seq, const std::string& msg, bool is_error) {
		// ordered_pending and next_expected_seq stored as members in Logger
		std::lock_guard<std::mutex> lock(ordered_mutex);
		if (seq == next_expected_seq) {
			// Ready now: push into ordered_ready_queue (written before normal queue)
			ordered_ready_queue.emplace_back(msg, is_error);
			pending_writes.fetch_add(1, std::memory_order_acq_rel);
			// Wake background writer
			log_queue_cv.notify_one();
			++next_expected_seq;
			// Flush any contiguous pending entries into ready queue
			while (true) {
				auto it = ordered_pending.find(next_expected_seq);
				if (it == ordered_pending.end()) break;
				auto entry = it->second;
				ordered_pending.erase(it);
				ordered_ready_queue.emplace_back(entry.first, entry.second);
				pending_writes.fetch_add(1, std::memory_order_acq_rel);
				log_queue_cv.notify_one();
				++next_expected_seq;
			}
		}
		else {
			// Store until its turn (do not increment pending_writes yet)
			ordered_pending.emplace(seq, std::make_pair(msg, is_error));
		}
	}

	uint64_t reserve_log_sequence() {
		return g_next_log_sequence.fetch_add(1, std::memory_order_relaxed);
	}

	void set_thread_log_sequence(uint64_t seq) {
		t_log_sequence = seq;
	}

	void clear_thread_log_sequence() {
		t_log_sequence.reset();
	}

	void enqueue_ordered(uint64_t seq, const std::string& msg, bool is_error) {
		if (auto g = get_global_logger()) {
			g->enqueue_ordered(seq, msg, is_error);
		}
		else {
			get_or_create_global_logger()->enqueue_log(msg, is_error);
		}
	}

	Logger* get_global_logger() {
		return global_logger_ref().load(std::memory_order_acquire);
	}

	void spawn_ordered(bud::threading::TaskScheduler* scheduler, std::move_only_function<void()> work, bud::threading::Counter* counter) {
		if (!scheduler) {
			// fallback: run directly on current thread
			uint64_t seq = reserve_log_sequence();
			set_thread_log_sequence(seq);
			work();
			clear_thread_log_sequence();
			return;
		}

		uint64_t seq = reserve_log_sequence();
		// move work into wrapper; set/clear thread-local sequence inside task
		scheduler->spawn([seq, w = std::move(work)]() mutable {
			set_thread_log_sequence(seq);
			w();
			clear_thread_log_sequence();
			}, counter);
	}

	Logger::Logger(bud::threading::TaskScheduler* scheduler) {
		backend_mask.store(LogBackend_Console | LogBackend_DebugOutput);

		// Store injected scheduler (may be nullptr)
		task_scheduler = scheduler;

		namespace fs = std::filesystem;
		auto get_exe_dir = []() -> fs::path {
#if defined(_WIN32)
			wchar_t buf[MAX_PATH];
			DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
			if (len == 0) return {};
			return fs::path(std::wstring(buf, buf + len)).parent_path();
#elif defined(__APPLE__)
			uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			if (size == 0) return {};
			std::string buf(size, '\0');
			if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
			std::error_code ec;
			return fs::weakly_canonical(fs::path(buf), ec).parent_path();
#else
			std::string buf(4096, '\0');
			ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size());
			if (len <= 0) return {};
			buf.resize(static_cast<size_t>(len));
			return fs::path(buf).parent_path();
#endif
			};

		auto exe_dir = get_exe_dir();
		if (!exe_dir.empty()) {
			// Create a timestamped log filename: bud_YYYYMMDD_HHMMSS.log
			auto make_timestamp = []() -> std::string {
				using namespace std::chrono;
				auto now = system_clock::now();
				std::time_t t = system_clock::to_time_t(now);
				std::tm tm_info{};
#ifdef _WIN32
				localtime_s(&tm_info, &t);
#else
				localtime_r(&t, &tm_info);
#endif
				std::ostringstream ss;
				ss << std::put_time(&tm_info, "%Y%m%d_%H%M%S");
				return ss.str();
				};

			auto ts = make_timestamp();
			auto log_path = exe_dir / ("bud_" + ts + ".log");
			// Store path and open file
			log_file_path = log_path.string();
			set_log_file(log_file_path);
		}
		// Publish this logger as the global non-owning instance so helpers (print/eprint)
		// can use it without forcing a singleton ownership model.
		set_global_logger(this);

		// Start background logging worker. If a TaskScheduler was injected, bind
		// the worker to a scheduler thread; otherwise create an internal std::thread
		// that will asynchronously drain the queue. All log output is performed
		// from the background worker to guarantee asynchronous behavior.
        bound_worker_index.reset();
        if (task_scheduler) {
            size_t cnt = task_scheduler->get_thread_count();
            if (cnt > 0) bound_worker_index = static_cast<uint32_t>(cnt - 1);
        }

		log_thread_running.store(true);
		if (task_scheduler && bound_worker_index) {
			uint32_t idx = *bound_worker_index;
			task_scheduler->spawn_on_thread(idx, "Logger.Background", [this]() {
				while (log_thread_running.load(std::memory_order_acquire)) {
					std::pair<std::string, bool> item;
					bool have_item = false;

					// Prefer ordered ready entries
					{
						std::lock_guard<std::mutex> ord_lock(ordered_mutex);
						if (!ordered_ready_queue.empty()) {
							item = ordered_ready_queue.front();
							ordered_ready_queue.pop_front();
							have_item = true;
						}
					}

					if (!have_item) {
						std::unique_lock<std::mutex> lk(log_queue_mutex);
						if (log_queue.empty()) {
							log_queue_cv.wait_for(lk, std::chrono::milliseconds(50));
						}
						if (!log_queue.empty()) {
							item = log_queue.front();
							log_queue.pop_front();
							have_item = true;
						}
					}

					if (!have_item) continue;

					// Perform output using this logger's backends
					uint32_t mask = backend_mask.load(std::memory_order_relaxed);
					if (mask & LogBackend_Console) {
						if (item.second) std::println(stderr, "{}", item.first);
						else std::println("{}", item.first);
					}
#ifdef _WIN32
					if (mask & LogBackend_DebugOutput) {
						OutputDebugStringA(item.first.c_str());
						OutputDebugStringA("\n");
					}
#endif
					if (mask & LogBackend_File) {
						std::lock_guard<std::mutex> lock(file_mutex);
						if (!log_file_path.empty()) {
							if (!file_stream.is_open()) file_stream.open(log_file_path, std::ios::app);
							if (file_stream) {
								file_stream << item.first << '\n';
								file_stream.flush();
							}
						}
						else {
							if (file_stream) { file_stream << item.first << '\n'; file_stream.flush(); }
						}
					}
					// decrement pending_writes
					pending_writes.fetch_sub(1, std::memory_order_acq_rel);
				}
				}, nullptr);
		}
		else {
			// Internal thread fallback (still asynchronous)
			worker_thread = std::thread([this]() {
				while (log_thread_running.load(std::memory_order_acquire)) {
					std::pair<std::string, bool> item;
					bool have_item = false;

					// Prefer ordered ready entries
					{
						std::lock_guard<std::mutex> ord_lock(ordered_mutex);
						if (!ordered_ready_queue.empty()) {
							item = ordered_ready_queue.front();
							ordered_ready_queue.pop_front();
							have_item = true;
						}
					}

					if (!have_item) {
						std::unique_lock<std::mutex> lk(log_queue_mutex);
						if (log_queue.empty()) {
							log_queue_cv.wait_for(lk, std::chrono::milliseconds(50));
						}
						if (!log_queue.empty()) {
							item = log_queue.front();
							log_queue.pop_front();
							have_item = true;
						}
					}

					if (!have_item) continue;

					uint32_t mask = backend_mask.load(std::memory_order_relaxed);
					if (mask & LogBackend_Console) {
						if (item.second) std::println(stderr, "{}", item.first);
						else std::println("{}", item.first);
					}
#ifdef _WIN32
					if (mask & LogBackend_DebugOutput) {
						OutputDebugStringA(item.first.c_str());
						OutputDebugStringA("\n");
					}
#endif
					if (mask & LogBackend_File) {
						std::lock_guard<std::mutex> lock(file_mutex);
						if (!log_file_path.empty()) {
							if (!file_stream.is_open()) file_stream.open(log_file_path, std::ios::app);
							if (file_stream) {
								file_stream << item.first << '\n';
								file_stream.flush();
							}
						}
						else {
							if (file_stream) { file_stream << item.first << '\n'; file_stream.flush(); }
						}
					}
					pending_writes.fetch_sub(1, std::memory_order_acq_rel);
				}
				});
		}
	}
	Logger::~Logger() {
		// Ensure pending async writes are finished before closing
		try {
			flush();
		}
		catch (...) {}

		// Stop background log task if running
		log_thread_running.store(false);
		log_queue_cv.notify_all();

		// Join internal worker thread if we created one
		if (worker_thread.joinable()) {
			worker_thread.join();
		}

		// Unpublish global pointer to avoid other code accessing this object during
		// teardown.
		set_global_logger(nullptr);

		if (file_stream.is_open()) file_stream.close();
	}

	void Logger::set_backend_mask(uint32_t mask) { backend_mask.store(mask, std::memory_order_relaxed); }
	uint32_t Logger::get_backend_mask() const { return backend_mask.load(std::memory_order_relaxed); }

	void Logger::set_log_file(const std::string& path) {
		std::lock_guard<std::mutex> lock(file_mutex);
		if (file_stream.is_open()) file_stream.close();
		if (!path.empty()) {
			log_file_path = path;
			file_stream.open(path, std::ios::app);
			if (file_stream) backend_mask.fetch_or(LogBackend_File, std::memory_order_relaxed);
			else backend_mask.fetch_and(~LogBackend_File, std::memory_order_relaxed);
		}
		else {
			log_file_path.clear();
			backend_mask.fetch_and(~LogBackend_File, std::memory_order_relaxed);
		}
	}

	void Logger::enqueue_log(const std::string& msg, bool is_error) {
		// Always enqueue the message for asynchronous processing by the
		// background worker (either scheduler-bound or internal thread).
		{
			std::lock_guard<std::mutex> lk(log_queue_mutex);
			log_queue.emplace_back(msg, is_error);
			pending_writes.fetch_add(1, std::memory_order_acq_rel);
		}
		log_queue_cv.notify_one();
	}

	void Logger::log(const std::string& msg) {
		enqueue_log(msg, false);
	}


	void Logger::log_error(const std::string& msg) {
		enqueue_log(std::string("[ERROR] ") + msg, true);
	}

	void Logger::flush() {
		// If an engine-injected scheduler exists and the current thread's
		// scheduler matches it, use wait_for_counter which integrates with
		// the scheduler's cooperative model. Otherwise fall back to polling.
		if (task_scheduler && bud::threading::t_scheduler == task_scheduler) {
			task_scheduler->wait_for_counter(pending_writes);
			return;
		}

		while (pending_writes.load(std::memory_order_acquire) > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

} // namespace bud
