#pragma once

#include <string>
#include <format>
#include <print>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <map>
#include <atomic>
#include <fstream>
#include <mutex>
#include <deque>
#include <cstdint>
#include <functional>
#include <condition_variable>
#include <optional>
#include <memory>

#include "src/threading/bud.threading.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif

namespace bud {
    std::string make_log_prefix();

    // Simple runtime-selectable logging backends (debug builds only)
    enum LogBackend : uint32_t {
        LogBackend_Console = 1 << 0,
        LogBackend_DebugOutput = 1 << 1, // OutputDebugString on Windows
        LogBackend_File = 1 << 2
    };

    class Logger {
    public:
        // Constructible logger; ownership should be held by the engine (std::unique_ptr).
        Logger();

        void set_backend_mask(uint32_t mask);
        uint32_t get_backend_mask() const;

        // Set file path; empty path closes file and disables file backend
        void set_log_file(const std::string& path);
        // Optional injected TaskScheduler for binding background worker to the
        // engine's thread pool. If nullptr, the logger will create an internal
        // background thread to ensure all logging remains asynchronous.
        Logger(bud::threading::TaskScheduler* scheduler);

        // Block until all pending asynchronous log writes are finished
        void flush();

        void log(const std::string& msg);
        void log_error(const std::string& msg);
        // Internal: enqueue a log message for background processing
        void enqueue_log(const std::string& msg, bool is_error);
        // Ordered logging: enqueue a log entry tied to a sequence id.
        void enqueue_ordered(uint64_t seq, const std::string& msg, bool is_error = false);

        ~Logger();

        std::atomic<uint32_t> backend_mask{0};
        std::ofstream file_stream;
        std::mutex file_mutex;
        // Path to the log file (used by async append helper)
        std::string log_file_path;
        // Pending asynchronous write counter
        bud::threading::Counter pending_writes{0};
        // Optional injected TaskScheduler (owned by the engine)
        bud::threading::TaskScheduler* task_scheduler = nullptr;
        // Background logging queue and synchronization (always used; logger
        // guarantees asynchronous processing via injected scheduler or an
        // internal thread)
        std::deque<std::pair<std::string, bool>> log_queue; // pair<msg, is_error>
        std::mutex log_queue_mutex;
        std::condition_variable log_queue_cv;
        std::atomic<bool> log_thread_running{ false };
        // Internal worker thread used when no TaskScheduler is injected.
        std::thread worker_thread;
        // Index of the worker thread this logger binds to (if any)
        std::optional<uint32_t> bound_worker_index;
        // Ordered logging state (protected by ordered_mutex)
        std::mutex ordered_mutex;
        std::map<uint64_t, std::pair<std::string, bool>> ordered_pending;
        std::deque<std::pair<std::string, bool>> ordered_ready_queue;
        uint64_t next_expected_seq = 0;
    };

    // Global non-owning accessor for the engine-managed logger instance. These
    // are intentionally non-owning so the engine keeps ownership semantics clear.
    void set_global_logger(Logger* logger);
    Logger* get_global_logger();

    // Ordered logging support
    // Reserve a monotonically increasing sequence id for a task before it is
    // spawned. The scheduler should call this on the spawning (main) thread
    // and store the id into the Fiber so that logs emitted by that task can
    // be written in spawn order.
    uint64_t reserve_log_sequence();

    // Bind the current thread to a log sequence (typically called at the
    // start of a worker fiber) so subsequent calls to `print` / `eprint`
    // will be routed to the ordered queue. Call `clear_thread_log_sequence`
    // after the task finishes.
    void set_thread_log_sequence(uint64_t seq);
    void clear_thread_log_sequence();

    // Enqueue an ordered log entry. Exported for completeness; normally
    // `print`/`eprint` route here when a thread-local sequence is set.
    void enqueue_ordered(uint64_t seq, const std::string& msg, bool is_error = false);

    // Convenience helper: schedule a task on the provided TaskScheduler such that
    // all logs emitted by the task are routed through an ordered sequence
    // corresponding to the spawn order. This avoids modifying the scheduler
    // internals while providing ordered logging for selected tasks.
    void spawn_ordered(bud::threading::TaskScheduler* scheduler, std::move_only_function<void()> work, bud::threading::Counter* counter = nullptr);

    // Ensure there is always a globally accessible logger. If none exists,
    // allocate a minimal emergency logger that uses an internal background
    // thread to keep all output asynchronous.
    inline Logger* get_or_create_global_logger() {
        if (auto g = get_global_logger()) return g;
        static std::atomic<Logger*> emergency{ nullptr };
        Logger* cur = emergency.load(std::memory_order_acquire);
        if (cur) return cur;
        std::unique_ptr<Logger> created_uptr = std::make_unique<Logger>(nullptr);
        // Emergency logger should not create or write to a permanent log file;
        // disable file backend for the emergency instance to avoid duplicate
        // file outputs once the real logger is installed.
        created_uptr->set_log_file("");

        Logger* created = created_uptr.release();
        Logger* expected = nullptr;
        if (!emergency.compare_exchange_strong(expected, created)) {
            delete created;
        }
        return emergency.load(std::memory_order_acquire);
    }

    // templated helpers (must be in header)
    template<typename... Args>
    inline void print(std::format_string<Args...> fmt, Args&&... args) {
        std::string body = std::format(fmt, std::forward<Args>(args)...);
        std::string msg = make_log_prefix() + body;
        // If a thread-local ordered sequence is set, route the message
        // through ordered enqueue so logs preserve spawn order.
        extern thread_local std::optional<uint64_t> t_log_sequence;
        if (t_log_sequence.has_value()) {
            enqueue_ordered(*t_log_sequence, msg, false);
            return;
        }

        if (auto g = get_global_logger()) {
            g->log(msg);
        } else {
            get_or_create_global_logger()->log(msg);
        }
    }
    template<typename... Args>
    inline void eprint(std::format_string<Args...> fmt, Args&&... args) {
        std::string body = std::format(fmt, std::forward<Args>(args)...);
        // Let Logger::log_error add the "[ERROR] " prefix so we don't duplicate it.
        std::string msg = make_log_prefix() + body;
        extern thread_local std::optional<uint64_t> t_log_sequence;
        if (t_log_sequence.has_value()) {
            enqueue_ordered(*t_log_sequence, msg, true);
            return;
        }

        if (auto g = get_global_logger()) {
            g->log_error(msg);
        } else {
            get_or_create_global_logger()->log_error(msg);
        }
    }

    inline void set_log_backend_mask(uint32_t mask) { if (auto g = get_global_logger()) g->set_backend_mask(mask); }
    inline uint32_t get_log_backend_mask() { return get_global_logger() ? get_global_logger()->get_backend_mask() : 0; }
    inline void set_log_file(const std::string& path) { if (auto g = get_global_logger()) g->set_log_file(path); }
}

// Thread-local sequence id used by print/eprint to route ordered logs.
// Place inside namespace `bud` so translation units can refer to `bud::t_log_sequence`.
namespace bud {
    inline thread_local std::optional<uint64_t> t_log_sequence{std::nullopt};
}
