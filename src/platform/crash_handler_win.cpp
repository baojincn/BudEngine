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
    write_minidump(exinfo);
    // Let default handler run after
    return EXCEPTION_EXECUTE_HANDLER;
}

static void terminate_handler() {
    // Attempt to capture a dump on std::terminate(). Avoid allocator use and
    // project logger because the C++ runtime may be in an invalid state.
    const CHAR term_msg[] = "std::terminate called - attempting to write minidump\n";
    OutputDebugStringA(term_msg);
    write_minidump(nullptr);
    // Ensure termination
    abort();
}

void install_crash_handler() {
    SetUnhandledExceptionFilter(UnhandledExceptionFilterImpl);
    std::set_terminate(terminate_handler);
}

} // namespace bud::platform

#endif // _WIN32
