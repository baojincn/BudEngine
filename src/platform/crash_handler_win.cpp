#ifdef _WIN32

#include "src/platform/crash_handler.hpp"
#include "src/io/bud.io.hpp"
#include <windows.h>
#include <DbgHelp.h>
#include <filesystem>
#include <string>
#include <chrono>
#include <format>
#include <sstream>
#include "src/core/bud.logger.hpp"

#pragma comment(lib, "Dbghelp.lib")

namespace bud::platform {

static std::string make_dump_filename() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_info{};
    localtime_s(&tm_info, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm_info, "%Y%m%d_%H%M%S") << "_" << GetCurrentProcessId() << ".dmp";
    return ss.str();
}

static void write_minidump(EXCEPTION_POINTERS* exinfo) {
    try {
        namespace fs = std::filesystem;
        fs::path tmp = fs::current_path() / "tmp";
        std::error_code ec;
        fs::create_directories(tmp, ec);
        auto name = make_dump_filename();
        fs::path dump_path = tmp / name;

        HANDLE hFile = CreateFileA(dump_path.string().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            // Use async logger to record the failure.
            DWORD createErr = GetLastError();
            std::string err = "Failed to create dump file: " + dump_path.string() +
                " (GetLastError=" + std::to_string(createErr) + ")\n";
            bud::get_or_create_global_logger()->log(err);
            return;
        }

        MINIDUMP_EXCEPTION_INFORMATION mei{};
        if (exinfo) {
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = exinfo;
            mei.ClientPointers = FALSE;
        }

        DWORD dumpType = MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithFullMemory;
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, (MINIDUMP_TYPE)dumpType, exinfo ? &mei : nullptr, nullptr, nullptr);

        CloseHandle(hFile);

        if (ok) {
            std::string msg = "Crash dump written to " + dump_path.string() + "\n";
            bud::get_or_create_global_logger()->log(msg);
        } else {
            std::string msg = "MiniDumpWriteDump failed (GetLastError=" + std::to_string(GetLastError()) + "): " + dump_path.string() + "\n";
            bud::get_or_create_global_logger()->log(msg);
        }
    } catch (const std::exception& e) {
        bud::eprint("Exception while writing minidump: {}", e.what());
    } catch (...) {
        // best-effort only
    }
}

static LONG WINAPI UnhandledExceptionFilterImpl(EXCEPTION_POINTERS* exinfo) {
    write_minidump(exinfo);
    // Let default handler run after
    return EXCEPTION_EXECUTE_HANDLER;
}

static void terminate_handler() {
    // Attempt to capture a dump on std::terminate()
    // Use async logger to record termination; keep it asynchronous per project policy
    const char term_msg[] = "std::terminate called - attempting to write minidump\n";
    bud::get_or_create_global_logger()->log(std::string(term_msg, sizeof(term_msg) - 1));
    write_minidump(nullptr);
    std::abort();
}

void install_crash_handler() {
    SetUnhandledExceptionFilter(UnhandledExceptionFilterImpl);
    std::set_terminate(terminate_handler);
}

} // namespace bud::platform

#endif // _WIN32
