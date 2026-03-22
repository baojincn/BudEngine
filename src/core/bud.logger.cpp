#include "bud.logger.hpp"

#ifdef _WIN32
    #include <Windows.h>
#endif

#include <print>

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

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() {
    backend_mask.store(LogBackend_Console | LogBackend_DebugOutput);
}
Logger::~Logger() {
    if (file_stream.is_open()) file_stream.close();
}

void Logger::set_backend_mask(uint32_t mask) { backend_mask.store(mask, std::memory_order_relaxed); }
uint32_t Logger::get_backend_mask() const { return backend_mask.load(std::memory_order_relaxed); }

void Logger::set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(file_mutex);
    if (file_stream.is_open()) file_stream.close();
    if (!path.empty()) {
        file_stream.open(path, std::ios::app);
        if (file_stream) backend_mask.fetch_or(LogBackend_File, std::memory_order_relaxed);
        else backend_mask.fetch_and(~LogBackend_File, std::memory_order_relaxed);
    } else {
        backend_mask.fetch_and(~LogBackend_File, std::memory_order_relaxed);
    }
}

void Logger::log(const std::string& msg) {
    uint32_t mask = backend_mask.load(std::memory_order_relaxed);
    if (mask & LogBackend_Console) std::println("{}", msg);
    #ifdef _WIN32
    if (mask & LogBackend_DebugOutput) {
        OutputDebugStringA(msg.c_str());
        OutputDebugStringA("\n");
    }
    #endif
    if (mask & LogBackend_File) {
        std::lock_guard<std::mutex> lock(file_mutex);
        if (file_stream) {
            file_stream << msg << '\n';
            file_stream.flush();
        }
    }
}

void Logger::log_error(const std::string& msg) {
    uint32_t mask = backend_mask.load(std::memory_order_relaxed);
    if (mask & LogBackend_Console) std::println(stderr, "{}", msg);
    #ifdef _WIN32
    if (mask & LogBackend_DebugOutput) {
        OutputDebugStringA(msg.c_str());
        OutputDebugStringA("\n");
    }
    #endif
    if (mask & LogBackend_File) {
        std::lock_guard<std::mutex> lock(file_mutex);
        if (file_stream) {
            file_stream << msg << '\n';
            file_stream.flush();
        }
    }
}

} // namespace bud
