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
        static Logger& instance();

        void set_backend_mask(uint32_t mask);
        uint32_t get_backend_mask() const;

        // Set file path; empty path closes file and disables file backend
        void set_log_file(const std::string& path);

        void log(const std::string& msg);
        void log_error(const std::string& msg);

    private:
        Logger();
        ~Logger();

        std::atomic<uint32_t> backend_mask{0};
        std::ofstream file_stream;
        std::mutex file_mutex;
    };

    // templated helpers (must be in header)
    template<typename... Args>
    inline void print(std::format_string<Args...> fmt, Args&&... args) {
        std::string body = std::format(fmt, std::forward<Args>(args)...);
        std::string msg = make_log_prefix() + body;
        Logger::instance().log(msg);
    }
    template<typename... Args>
    inline void eprint(std::format_string<Args...> fmt, Args&&... args) {
        std::string body = std::format(fmt, std::forward<Args>(args)...);
        std::string msg = make_log_prefix() + std::string("[ERROR] ") + body;
        Logger::instance().log_error(msg);
    }

    inline void set_log_backend_mask(uint32_t mask) { Logger::instance().set_backend_mask(mask); }
    inline uint32_t get_log_backend_mask() { return Logger::instance().get_backend_mask(); }
    inline void set_log_file(const std::string& path) { Logger::instance().set_log_file(path); }
}
