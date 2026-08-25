#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace bud::asset_pipeline::support {

struct ProcessResult {
    int exit_code = 0;
    std::string stdout_str;
    std::string stderr_str;
};

// Binary file reading helper
std::optional<std::vector<char>> read_binary_file(const std::filesystem::path& path);

// Atomic text file write helper (write to temp then replace)
bool write_text_file_atomic(const std::filesystem::path& path, const std::string& text);

// Subprocess execution and output capture
ProcessResult run_process_capture(const std::string& command, const std::filesystem::path& working_directory = {}, unsigned int timeout_ms = 0);

// Simple logging
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// Environment variable reader
std::optional<std::string> get_env_var(const std::string& name);

} // namespace bud::asset_pipeline::support
