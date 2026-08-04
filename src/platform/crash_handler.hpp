#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

namespace bud::platform {
    // Install platform-specific crash handlers (writes a minidump into tmp/ on Windows).
    void install_crash_handler();
    // Optional: provide an engine/root path so crash dumps can be written under the
    // project tree (e.g. <root>/tmp/...). The argument is copied; caller may pass
    // a pointer to a nul-terminated UTF-8/ANSI path.
    void set_crash_dump_root(const char* root_path);
}
