// Crashes on purpose, for the crash handler test. It is copied to plugin
// paths (a CMFM plugin folder, a Stracker plugin folder) before it loads.
#include <windows.h>

namespace {
DWORD WINAPI crash_thread(void*) {
    volatile int* nowhere = nullptr;
    *nowhere = 7;
    return 0;
}
}  // namespace

// A thread this DLL starts writes through a null pointer.
extern "C" __declspec(dllexport) void crash_on_thread() {
    if (const HANDLE thread = CreateThread(nullptr, 0, crash_thread, nullptr, 0, nullptr)) CloseHandle(thread);
}

// Windows code faults on a bad pointer this DLL passes; the DLL is only on
// the stack. The store after the call keeps this frame on the stack.
extern "C" __declspec(dllexport) void crash_in_windows() {
    volatile int after = 0;
    GetSystemTime(reinterpret_cast<LPSYSTEMTIME>(8));
    after = 1;
}
