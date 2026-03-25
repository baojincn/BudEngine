#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

namespace bud::platform {
    // Install platform-specific crash handlers (writes a minidump into tmp/ on Windows).
    void install_crash_handler();
}
