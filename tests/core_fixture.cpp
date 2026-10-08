#include <windows.h>

namespace { HANDLE g_exe_lock = INVALID_HANDLE_VALUE; }

extern "C" __declspec(dllexport) DWORD WINAPI cmfm_bootstrap(void*) noexcept {
#if CMFM_FIXTURE_MODE == 4
    // Hold a read handle denying other read opens until the stand-in exits.
    // This produces a real post-resume checksum read failure without a
    // production fault-injection option or changing any executable on disk.
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])))) {
        return ERROR_OPEN_FAILED;
    }
    g_exe_lock = CreateFileW(exe, GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_exe_lock == INVALID_HANDLE_VALUE) return ERROR_OPEN_FAILED;
#endif
    return ERROR_NOT_SUPPORTED;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
#if CMFM_FIXTURE_MODE == 2
        return FALSE;
#elif CMFM_FIXTURE_MODE == 3
        Sleep(INFINITE);  // Intentional fault fixture, never part of the distribution.
#endif
    }
    if (reason == DLL_PROCESS_DETACH && g_exe_lock != INVALID_HANDLE_VALUE) CloseHandle(g_exe_lock);
    return TRUE;
}
