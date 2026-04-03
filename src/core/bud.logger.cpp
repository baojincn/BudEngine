#include "bud.logger.hpp"
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bud {

	static std::atomic<Logger*> g_active_logger{ nullptr };

	void set_global_logger(Logger* logger) {
		g_active_logger.store(logger, std::memory_order_release);
	}

	Logger* get_global_logger() {
		return g_active_logger.load(std::memory_order_acquire);
	}

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
		ss << " [TID=" << std::setw(6) << (tid_hash % 1000000) << "] ";
		return ss.str();
	}

	Logger::Logger(const std::filesystem::path& root_dir) {
		backend_mask.store(LogBackend_Console | LogBackend_DebugOutput);

		// Ensure logs are written under a `log/` directory. If `root_dir` is empty,
		// fall back to the current working directory. Create the directory if it
		// does not already exist.
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
		std::filesystem::path dir;
		if (!root_dir.empty()) {
			dir = root_dir / "log";
		} else {
			dir = std::filesystem::current_path() / "log";
		}

		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) {
			bud::eprint("[Logger] Failed to create log directory '{}': {}", dir.string(), ec.message());
		} else {
			auto log_path = dir / ("bud_" + ts + ".log");
			set_log_file(log_path.string());
		}

		is_running.store(true, std::memory_order_release);
		logger_thread = std::thread(&Logger::io_thread_loop, this);
	}

	Logger::~Logger() {
		stop();
	}

	void Logger::set_backend_mask(uint32_t mask) {
		backend_mask.store(mask, std::memory_order_relaxed);
	}

	uint32_t Logger::get_backend_mask() const {
		return backend_mask.load(std::memory_order_relaxed);
	}

	void Logger::set_log_file(const std::string& path) {
		std::lock_guard<std::mutex> lock(log_writter_mutex);
		if (file_stream.is_open()) {
			file_stream.close();
		}

		if (!path.empty()) {
			// Ensure parent directory exists
			std::filesystem::path p(path);
			std::filesystem::path dir = p.parent_path();
			if (!dir.empty()) {
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				if (ec) {
					bud::eprint("[Logger] Failed to create log directory '{}': {}", dir.string(), ec.message());
				}
			}

			log_file_path = path;
			file_stream.open(path, std::ios::app);
			if (file_stream) {
				backend_mask.fetch_or(LogBackend_File, std::memory_order_relaxed);
			} else {
				backend_mask.fetch_and(~LogBackend_File, std::memory_order_relaxed);
				std::println(stderr, "[Logger] FATAL: Can't open : {}", path);
			}
		}
		else {
			log_file_path.clear();
			backend_mask.fetch_and(~LogBackend_File, std::memory_order_relaxed);
		}
	}

	void Logger::enqueue_log(const std::string& msg, bool is_error) {
		if (!is_running.load(std::memory_order_acquire)) return;

		std::string full_msg = make_log_prefix() + msg;

		{
			std::lock_guard<std::mutex> lock(log_writter_mutex);
			if (!is_running.load(std::memory_order_acquire)) return;
			log_queue.push_back({ std::move(full_msg), is_error });
		}

		resource_condition_guard.notify_one();
	}

	void Logger::log(const std::string& msg) {
		enqueue_log(msg, false);
	}

	void Logger::log_error(const std::string& msg) {
		enqueue_log(std::string("[ERROR] ") + msg, true);
	}

	void Logger::flush() {
		while (true) {
			{
				std::lock_guard<std::mutex> lock(log_writter_mutex);
				if (log_queue.empty()) break;
			}
			std::this_thread::yield();
		}

		std::lock_guard<std::mutex> lock(log_writter_mutex);
		if (file_stream.is_open()) {
			file_stream.flush();
		}
	}

	void Logger::stop() {
		if (!is_running.exchange(false, std::memory_order_acq_rel))
			return; // 防止重复 stop

		resource_condition_guard.notify_all();

		if (logger_thread.joinable()) {
			logger_thread.join(); // 强行按住主线程，直到所有遗留日志写完
		}

		if (file_stream.is_open()) {
			file_stream.close();
		}
	}

	void Logger::io_thread_loop() {
		std::vector<LogMessage> local_queue;
		local_queue.reserve(128);

		while (true) {
			{
				std::unique_lock<std::mutex> lock(log_writter_mutex);
				resource_condition_guard.wait(lock, [this]() {
					return !log_queue.empty() || !is_running.load(std::memory_order_acquire);
					});

				if (log_queue.empty() && !is_running.load(std::memory_order_acquire)) {
					break;
				}

				local_queue.swap(log_queue);
			}

			uint32_t mask = backend_mask.load(std::memory_order_relaxed);

			for (const auto& msg : local_queue) {
				if (mask & LogBackend_Console) {
					if (msg.is_error) {
						std::println(stderr, "{}", msg.text);
					}
					else {
						std::println("{}", msg.text);
					}
				}

				if (mask & LogBackend_DebugOutput) {
#ifdef _WIN32
					OutputDebugStringA((msg.text + "\n").c_str());
#endif
				}

				if (mask & LogBackend_File) {
					if (file_stream.is_open()) {
						file_stream << msg.text << '\n';
					}
				}
			}

			if ((mask & LogBackend_File) && file_stream.is_open() && !local_queue.empty()) {
				file_stream.flush();
			}

			local_queue.clear();
		}
	}

} // namespace bud
