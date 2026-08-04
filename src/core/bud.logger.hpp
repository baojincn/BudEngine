#pragma once

#include <string>
#include <format>
#include <print>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <cstdint>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

namespace bud {

	std::string make_log_prefix();

	enum LogBackend : uint32_t {
		LogBackend_Console = 1 << 0,
		LogBackend_DebugOutput = 1 << 1, // OutputDebugString on Windows
		LogBackend_File = 1 << 2
	};

	class Logger {
	public:
		explicit Logger(const std::filesystem::path& root_dir);
		~Logger();

		Logger(const Logger&) = delete;
		Logger& operator=(const Logger&) = delete;
		Logger(Logger&&) = delete;
		Logger& operator=(Logger&&) = delete;

		void set_backend_mask(uint32_t mask);
		uint32_t get_backend_mask() const;
		void set_log_file(const std::string& path);

		void flush();
		void stop();

		void log(const std::string& msg);
		void log_error(const std::string& msg);
		void enqueue_log(const std::string& msg, bool is_error);

	private:
		void io_thread_loop();

		struct LogMessage {
			std::string text;
			bool is_error;
		};

		std::atomic<uint32_t> backend_mask{ 0 };
		std::string log_file_path;
		std::ofstream file_stream;

		std::mutex log_writter_mutex;
		std::condition_variable resource_condition_guard;
		std::vector<LogMessage> log_queue;
		std::atomic<bool> is_running{ false };

		std::thread logger_thread;
	};

	// 声明全局访问点
	void set_global_logger(Logger* logger);
	Logger* get_global_logger();

	template<typename... Args>
	inline void print(std::format_string<Args...> fmt, Args&&... args) {
		std::string body = std::format(fmt, std::forward<Args>(args)...);
		if (auto g = get_global_logger()) {
			g->log(body);
		}
		else {
			// 引擎没启动或已崩溃时，直接输出到控制台
			std::println("{}", body);
		}
	}

	template<typename... Args>
	inline void eprint(std::format_string<Args...> fmt, Args&&... args) {
		std::string body = std::format(fmt, std::forward<Args>(args)...);
		if (auto g = get_global_logger()) {
			g->log_error(body);
		}
		else {
			std::println(stderr, "[ERROR] {}", body);
		}
	}

} // namespace bud
