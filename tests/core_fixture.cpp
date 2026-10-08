#include <windows.h>

extern "C" __declspec(dllexport) DWORD WINAPI cmfm_bootstrap(void*) noexcept {
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
    return TRUE;
}
