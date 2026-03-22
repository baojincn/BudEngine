#include "bud_tool_support.hpp"
#include <fstream>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <chrono>
#endif
#include <thread>

namespace bud::tool_support {

std::optional<std::vector<char>> read_binary_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return std::nullopt;
    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) return std::vector<char>{};
    std::vector<char> buf((size_t)size);
    if (!in.read(buf.data(), size)) return std::nullopt;
    return buf;
}

bool write_text_file_atomic(const std::filesystem::path& p, const std::string& text) {
    try {
        auto tmp = p;
        tmp += ".tmp";
        std::ofstream out(tmp, std::ios::binary);
        if (!out.is_open()) return false;
        out.write(text.data(), text.size());
        out.close();
        std::error_code ec;
        std::filesystem::rename(tmp, p, ec);
        if (ec) {
            // fallback to copy
            std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(tmp, ec);
            return ec.value() == 0;
        }
        return true;
    } catch (...) {
        return false;
    }
}

ProcessResult run_process_capture(const std::string& command, const std::filesystem::path& working_directory, unsigned int timeout_ms) {
    ProcessResult res{};
#if defined(_WIN32)
    // Windows implementation using CreateProcess and redirected pipes
    HANDLE stdout_read = NULL, stdout_write = NULL;
    HANDLE stderr_read = NULL, stderr_write = NULL;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) { res.exit_code = -1; return res; }
    if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0)) { CloseHandle(stdout_read); CloseHandle(stdout_write); res.exit_code = -1; return res; }

    // Ensure read handles are not inheritable
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi{};

    // Create mutable command line
    std::string cmd = command;
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
        working_directory.empty() ? nullptr : working_directory.string().c_str(), &si, &pi);

    // Close write handles in parent
    CloseHandle(stdout_write); CloseHandle(stderr_write);

    if (!ok) {
        CloseHandle(stdout_read); CloseHandle(stderr_read);
        res.exit_code = -1;
        return res;
    }

    // Read output asynchronously
    std::string out_s, err_s;
    std::thread out_thread([&]{
        char buf[4096];
        DWORD read = 0;
        while (true) {
            if (!ReadFile(stdout_read, buf, sizeof(buf), &read, nullptr) || read == 0) break;
            out_s.append(buf, buf + read);
        }
    });
    std::thread err_thread([&]{
        char buf[4096];
        DWORD read = 0;
        while (true) {
            if (!ReadFile(stderr_read, buf, sizeof(buf), &read, nullptr) || read == 0) break;
            err_s.append(buf, buf + read);
        }
    });

    DWORD wait_ms = timeout_ms == 0 ? INFINITE : timeout_ms;
    DWORD wait_res = WaitForSingleObject(pi.hProcess, wait_ms);
    if (wait_res == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        res.exit_code = -1;
    } else {
        DWORD exit_code = 0;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        res.exit_code = static_cast<int>(exit_code);
    }

    // Close process handles and join readers
    out_thread.join(); err_thread.join();
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    CloseHandle(stdout_read); CloseHandle(stderr_read);

    res.stdout_str = out_s;
    res.stderr_str = err_s;
    return res;
#else
    // POSIX implementation using fork/exec and non-blocking pipes
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) != 0) { res.exit_code = -1; return res; }
    if (pipe(err_pipe) != 0) { close(out_pipe[0]); close(out_pipe[1]); res.exit_code = -1; return res; }

    pid_t pid = fork();
    if (pid == 0) {
        // child
        if (!working_directory.empty()) {
            chdir(working_directory.string().c_str());
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)NULL);
        _exit(127);
    }

    // parent
    close(out_pipe[1]); close(err_pipe[1]);
    // set non-blocking
    int flags = fcntl(out_pipe[0], F_GETFL, 0); fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(err_pipe[0], F_GETFL, 0); fcntl(err_pipe[0], F_SETFL, flags | O_NONBLOCK);

    std::string out_s, err_s;
    bool exited = false;
    int status = 0;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        // read available data
        char buf[4096];
        ssize_t r = read(out_pipe[0], buf, sizeof(buf));
        if (r > 0) out_s.append(buf, buf + r);
        r = read(err_pipe[0], buf, sizeof(buf));
        if (r > 0) err_s.append(buf, buf + r);

        // check child status
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) { exited = true; }

        if (exited) break;

        if (timeout_ms > 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= (long long)timeout_ms) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                exited = true;
                break;
            }
        }

        // sleep briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // drain remaining
    char buf[4096];
    ssize_t r;
    while ((r = read(out_pipe[0], buf, sizeof(buf))) > 0) out_s.append(buf, buf + r);
    while ((r = read(err_pipe[0], buf, sizeof(buf))) > 0) err_s.append(buf, buf + r);

    close(out_pipe[0]); close(err_pipe[0]);

    if (WIFEXITED(status)) res.exit_code = WEXITSTATUS(status);
    else res.exit_code = -1;
    res.stdout_str = out_s;
    res.stderr_str = err_s;
    return res;
#endif
}

void log_info(const std::string& msg) {
    std::cout << msg << std::endl;
}
void log_warn(const std::string& msg) {
    std::cerr << msg << std::endl;
}
void log_error(const std::string& msg) {
    std::cerr << msg << std::endl;
}

std::optional<std::string> get_env_var(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
}

} // namespace bud::tool_support
