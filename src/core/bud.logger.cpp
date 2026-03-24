#include "bud.logger.hpp"

#ifdef _WIN32
    #include <Windows.h>
#endif

#include <print>
#include <filesystem>
#include "src/threading/bud.threading.hpp"
#include "src/io/bud.io.hpp"

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
    static std::atomic<Logger*> ptr{nullptr};
    return ptr;
}

void set_global_logger(Logger* logger) {
    global_logger_ref().store(logger, std::memory_order_release);
}

Logger* get_global_logger() {
    return global_logger_ref().load(std::memory_order_acquire);
}


// Note: TaskScheduler may be provided at construction time. This function
// removed to prefer injection via constructor.

Logger::Logger(bud::threading::TaskScheduler* scheduler) {
    backend_mask.store(LogBackend_Console | LogBackend_DebugOutput);

    // Store injected scheduler (may be nullptr)
    task_scheduler = scheduler;

    // Attempt to open a log file next to the executable so running the
    // binary from the filesystem records logs in the exe directory.
    try {
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
    } catch (...) {
        // Best-effort: do not throw from logger construction
    }
    // Publish this logger as the global non-owning instance so helpers (print/eprint)
    // can use it without forcing a singleton ownership model.
    set_global_logger(this);
}
Logger::~Logger() {
    // Ensure pending async writes are finished before closing
    try {
        flush();
    } catch (...) {}
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
    } else {
        log_file_path.clear();
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
        if (!log_file_path.empty()) {
            bud::io::FileSystem::append_text_async(log_file_path, msg, &pending_writes, task_scheduler);
        } else {
            std::lock_guard<std::mutex> lock(file_mutex);
            if (file_stream) {
                file_stream << msg << '\n';
                file_stream.flush();
            }
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
        if (!log_file_path.empty()) {
            bud::io::FileSystem::append_text_async(log_file_path, msg, &pending_writes, task_scheduler);
        } else {
            std::lock_guard<std::mutex> lock(file_mutex);
            if (file_stream) {
                file_stream << msg << '\n';
                file_stream.flush();
            }
        }
    }
}

void Logger::flush() {
    // If an engine-injected scheduler exists and the current thread's
    // scheduler matches it, use wait_for_counter which integrates with
    // the scheduler's cooperative model. Otherwise fall back to polling.
    if (task_scheduler && bud::threading::t_scheduler == task_scheduler) {
        try {
            task_scheduler->wait_for_counter(pending_writes);
            return;
        } catch (...) {
            // Fall through to fallback polling
        }
    }

    while (pending_writes.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace bud
