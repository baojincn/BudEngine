#pragma once

#include <string>
#include <format>
#include <print>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <atomic>
#include <fstream>
#include <mutex>
#include <cstdint>
#include <functional>

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
        // Inject the engine's TaskScheduler for async log writes via constructor.
        // Passing nullptr disables async scheduling and falls back to synchronous writes.
        Logger(bud::threading::TaskScheduler* scheduler);

        // Block until all pending asynchronous log writes are finished
        void flush();

        void log(const std::string& msg);
        void log_error(const std::string& msg);

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
    };

    // Global non-owning accessor for the engine-managed logger instance. These
    // are intentionally non-owning so the engine keeps ownership semantics clear.
    void set_global_logger(Logger* logger);
    Logger* get_global_logger();

    // templated helpers (must be in header)
    template<typename... Args>
    inline void print(std::format_string<Args...> fmt, Args&&... args) {
        std::string body = std::format(fmt, std::forward<Args>(args)...);
        std::string msg = make_log_prefix() + body;
        if (auto g = get_global_logger()) {
            g->log(msg);
        } else {
            std::println("{}", msg);
        }
    }
    template<typename... Args>
    inline void eprint(std::format_string<Args...> fmt, Args&&... args) {
        std::string body = std::format(fmt, std::forward<Args>(args)...);
        std::string msg = make_log_prefix() + std::string("[ERROR] ") + body;
        if (auto g = get_global_logger()) {
            g->log_error(msg);
        } else {
            std::println(stderr, "{}", msg);
        }
    }

    inline void set_log_backend_mask(uint32_t mask) { if (auto g = get_global_logger()) g->set_backend_mask(mask); }
    inline uint32_t get_log_backend_mask() { return get_global_logger() ? get_global_logger()->get_backend_mask() : 0; }
    inline void set_log_file(const std::string& path) { if (auto g = get_global_logger()) g->set_log_file(path); }
}
