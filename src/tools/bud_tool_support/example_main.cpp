#include "bud_tool_support.hpp"
#include <filesystem>

int main() {
    using namespace bud::tool_support;

    log_info("bud_tool_support example starting");

    // ensure tmp dir exists
    std::filesystem::path tmp_dir = std::filesystem::current_path() / "tmp";
    std::error_code ec;
    std::filesystem::create_directories(tmp_dir, ec);
    if (ec) {
        log_error(std::string("Failed to create tmp dir: ") + ec.message());
        return 1;
    }

    // Write a text file atomically
    std::filesystem::path test_path = tmp_dir / "example.txt";
    std::string contents = "Hello from bud_tool_support example\n";
    if (!write_text_file_atomic(test_path, contents)) {
        log_error("Failed to write file atomically");
        return 2;
    }
    log_info(std::string("Wrote file: ") + test_path.string());

    // Read it back
    auto data_opt = read_binary_file(test_path);
    if (!data_opt) {
        log_error("Failed to read file back");
        return 3;
    }
    log_info(std::string("Read file size: ") + std::to_string(data_opt->size()));

    // Run a simple process and capture output
#ifdef _WIN32
    std::string cmd = "cmd /C echo bud_tool_support process test";
#else
    std::string cmd = "echo bud_tool_support process test";
#endif
    ProcessResult pr = run_process_capture(cmd);
    log_info(std::string("Process exit code: ") + std::to_string(pr.exit_code));
    log_info(std::string("Process stdout: ") + pr.stdout_str);
    if (!pr.stderr_str.empty()) log_warn(std::string("Process stderr: ") + pr.stderr_str);

    log_info("bud_tool_support example finished");
    return 0;
}
