#ifdef _WIN32

#include "src/platform/crash_handler.hpp"
#include <windows.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cstring>
#include <exception>

#pragma comment(lib, "Dbghelp.lib")

namespace bud::platform {
// Shared state for configured root path.
static CHAR g_crash_root[MAX_PATH] = {0};
static bool g_has_crash_root = false;

// Set the root path used for crash dumps. Call during initialization when
// the VFS is available. The string is copied into an internal buffer to
// avoid allocations during crash handling.
void set_crash_dump_root(const char* root_path) {
    if (!root_path) return;
    strncpy_s(g_crash_root, root_path, MAX_PATH - 1);
    g_has_crash_root = true;
    // Ensure the 'tmp' directory exists under the provided root.
    CHAR tmpdir[MAX_PATH];
    size_t len = strlen(g_crash_root);
    if (len > 0 && (g_crash_root[len - 1] == '\\' || g_crash_root[len - 1] == '/'))
        sprintf_s(tmpdir, "%s%s", g_crash_root, "tmp");
    else
        sprintf_s(tmpdir, "%s\\%s", g_crash_root, "tmp");
    CreateDirectoryA(tmpdir, nullptr);
}

// Minimal, allocator-free minidump writer. Avoids STL and project logger to
// reduce the chance of deadlocks when called from an unstable state.
static void write_minidump(EXCEPTION_POINTERS* exinfo) {
    CHAR tempPath[MAX_PATH] = {0};
    if (g_has_crash_root) {
        // Use configured root/tmp
        size_t len = strlen(g_crash_root);
        if (len > 0 && (g_crash_root[len - 1] == '\\' || g_crash_root[len - 1] == '/'))
            sprintf_s(tempPath, "%s%s", g_crash_root, "tmp");
        else
            sprintf_s(tempPath, "%s\\%s", g_crash_root, "tmp");
    } else {
        DWORD tpLen = GetTempPathA(MAX_PATH, tempPath);
        if (tpLen == 0 || tpLen > MAX_PATH) {
            // fallback to current directory
            strcpy_s(tempPath, ".\\");
        }
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    DWORD pid = GetCurrentProcessId();

    CHAR filename[MAX_PATH];
    sprintf_s(filename, "%04u%02u%02u_%02u%02u%02u_%03u_%u.dmp",
              (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
              (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
              (unsigned)st.wMilliseconds, (unsigned)pid);

    CHAR fullpath[MAX_PATH];
    size_t tlen = strlen(tempPath);
    if (tlen > 0 && (tempPath[tlen - 1] == '\\' || tempPath[tlen - 1] == '/')) {
        sprintf_s(fullpath, "%s%s", tempPath, filename);
    } else {
        sprintf_s(fullpath, "%s\\%s", tempPath, filename);
    }

    // Ensure the directory exists in case it wasn't created earlier
    CHAR dirpath[MAX_PATH];
    strncpy_s(dirpath, tempPath, MAX_PATH - 1);
    CreateDirectoryA(dirpath, nullptr);

    HANDLE hFile = CreateFileA(fullpath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        CHAR msg[512];
        sprintf_s(msg, "Failed to create dump file: %s (GetLastError=%lu)\n", fullpath, GetLastError());
        OutputDebugStringA(msg);
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    MINIDUMP_EXCEPTION_INFORMATION* pmei = nullptr;
    if (exinfo) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exinfo;
        mei.ClientPointers = FALSE;
        pmei = &mei;
    }

    DWORD dumpType = MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithFullMemory;
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, (MINIDUMP_TYPE)dumpType, pmei, nullptr, nullptr);

    CloseHandle(hFile);

    if (ok) {
        CHAR msg[512];
        sprintf_s(msg, "Crash dump written to %s\n", fullpath);
        OutputDebugStringA(msg);
    } else {
        CHAR msg[512];
        sprintf_s(msg, "MiniDumpWriteDump failed (GetLastError=%lu): %s\n", GetLastError(), fullpath);
        OutputDebugStringA(msg);
    }
}

static LONG WINAPI UnhandledExceptionFilterImpl(EXCEPTION_POINTERS* exinfo) {
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    if (exinfo && exinfo->ExceptionRecord) {
        char buf[256];
        sprintf_s(buf, "[CRASH] ExceptionCode=0x%08lX at Address=0x%p\n",
            exinfo->ExceptionRecord->ExceptionCode,
            exinfo->ExceptionRecord->ExceptionAddress);
        OutputDebugStringA(buf);
        fprintf(stderr, "%s", buf);
        fflush(stderr);
    }

    void* stack[32];
    unsigned short frames = CaptureStackBackTrace(0, 32, stack, nullptr);
    char symbolBuffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (unsigned int i = 0; i < frames; ++i) {
        SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD displacement = 0;
        if (SymGetLineFromAddr64(process, (DWORD64)(stack[i]), &displacement, &line)) {
            fprintf(stderr, "  [%u] %s (%s:%lu)\n", i, symbol->Name, line.FileName, line.LineNumber);
        } else {
            fprintf(stderr, "  [%u] %s (0x%p)\n", i, symbol->Name, stack[i]);
        }
        fflush(stderr);
    }

    write_minidump(exinfo);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void terminate_handler() {
    const CHAR term_msg[] = "std::terminate called - attempting to write minidump\n";
    OutputDebugStringA(term_msg);
    fprintf(stderr, "%s", term_msg);
    try {
        auto current_ex = std::current_exception();
        if (current_ex) {
            std::rethrow_exception(current_ex);
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[TERMINATE] std::exception: %s\n", e.what());
        fflush(stderr);
    } catch (...) {
        fprintf(stderr, "[TERMINATE] unknown exception\n");
        fflush(stderr);
    }
    write_minidump(nullptr);
    abort();
}

void install_crash_handler() {
    SetUnhandledExceptionFilter(UnhandledExceptionFilterImpl);
    std::set_terminate(terminate_handler);
}

} // namespace bud::platform

#endif // _WIN32
