#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace bud::tool_support {

struct ProcessResult {
    int exit_code;
    std::string stdout_str;
    std::string stderr_str;
};

// Read a binary file into a buffer
std::optional<std::vector<char>> read_binary_file(const std::filesystem::path& p);

// Atomically write text to file (write to temp then rename)
bool write_text_file_atomic(const std::filesystem::path& p, const std::string& text);

// Run a process and capture stdout/stderr (platform dependent internal implementation)
// Run a process and capture stdout/stderr. If timeout_ms > 0, kill process after timeout_ms milliseconds.
ProcessResult run_process_capture(const std::string& command, const std::filesystem::path& working_directory = {}, unsigned int timeout_ms = 0);

// Simple logging helpers
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// Utility to read environment variable
std::optional<std::string> get_env_var(const std::string& name);

} // namespace bud::tool_support
