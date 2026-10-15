#include "core/hooks/thread_snapshot.hpp"

#include <processsnapshot.h>
#include <atomic>
#include <bit>

namespace {
std::atomic<unsigned> g_attempts{0}, g_succeeded{0};

// Resolved at run time, never imported: Wine and Proton lack some or all of
// these, and there an unresolved import is a stub that ends the process when
// called. Without them the snapshot fails and MinHook's Toolhelp path runs.
struct Pss {
    decltype(&PssCaptureSnapshot) capture = nullptr;
    decltype(&PssQuerySnapshot) query = nullptr;
    decltype(&PssFreeSnapshot) free_snapshot = nullptr;
    decltype(&PssWalkMarkerCreate) marker_create = nullptr;
    decltype(&PssWalkMarkerFree) marker_free = nullptr;
    decltype(&PssWalkSnapshot) walk = nullptr;

    explicit Pss(HMODULE module) {
        if (!module) return;
        resolve(module, "PssCaptureSnapshot", capture);
        resolve(module, "PssQuerySnapshot", query);
        resolve(module, "PssFreeSnapshot", free_snapshot);
        resolve(module, "PssWalkMarkerCreate", marker_create);
        resolve(module, "PssWalkMarkerFree", marker_free);
        resolve(module, "PssWalkSnapshot", walk);
    }
    template <typename Function>
    static void resolve(HMODULE module, const char* name, Function& function) {
        function = std::bit_cast<Function>(GetProcAddress(module, name));
    }
    bool complete() const { return capture && query && free_snapshot && marker_create && marker_free && walk; }
};
struct Snapshot {
    const Pss& pss;
    HPSS value = nullptr;
    ~Snapshot() { if (value) pss.free_snapshot(GetCurrentProcess(), value); }
};
struct Marker {
    const Pss& pss;
    HPSSWALK value = nullptr;
    ~Marker() { if (value) pss.marker_free(value); }
};
struct Ids {
    HANDLE heap;
    LPDWORD value = nullptr;
    ~Ids() { if (value) HeapFree(heap, 0, value); }
};
}

BOOL al::hooks::enumerate_threads_from(HMODULE module, HANDLE heap, LPDWORD* ids, UINT* count) noexcept {
    g_attempts.fetch_add(1);
    *ids = nullptr;
    *count = 0;
    const Pss pss(module);
    if (!pss.complete()) return FALSE;
    Snapshot snapshot{pss};
    // IDs only: no contexts, virtual memory, handles or clone process captured.
    if (pss.capture(GetCurrentProcess(), PSS_CAPTURE_THREADS, 0, &snapshot.value) != ERROR_SUCCESS) return FALSE;
    PSS_THREAD_INFORMATION info{};
    if (pss.query(snapshot.value, PSS_QUERY_THREAD_INFORMATION, &info, sizeof(info)) != ERROR_SUCCESS) return FALSE;
    Marker marker{pss};
    if (pss.marker_create(nullptr, &marker.value) != ERROR_SUCCESS) return FALSE;
    Ids result{heap};
    if (info.ThreadsCaptured) {
        result.value = static_cast<LPDWORD>(HeapAlloc(heap, 0, static_cast<SIZE_T>(info.ThreadsCaptured) * sizeof(DWORD)));
        if (!result.value) return FALSE;
    }
    UINT size = 0;
    const auto process = GetCurrentProcessId();
    const auto self = GetCurrentThreadId();
    for (;;) {
        PSS_THREAD_ENTRY thread{};
        const auto status = pss.walk(snapshot.value, PSS_WALK_THREADS, marker.value, &thread, sizeof(thread));
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) return FALSE;
        if (thread.ProcessId != process || thread.ThreadId == self) continue;
        if (!thread.ThreadId || size >= info.ThreadsCaptured) return FALSE;
        result.value[size++] = thread.ThreadId;
    }
    *count = size;
    *ids = result.value;
    result.value = nullptr;
    g_succeeded.fetch_add(1);
    return TRUE;
}

extern "C" BOOL WINAPI al_enumerate_process_threads(HANDLE heap, LPDWORD* ids, UINT* count) noexcept {
    return al::hooks::enumerate_threads_from(GetModuleHandleW(L"kernel32.dll"), heap, ids, count);
}

al::hooks::ThreadSnapshotCounts al::hooks::thread_snapshot_counts() noexcept {
    const auto succeeded = g_succeeded.load();
    return {g_attempts.load(), succeeded};
}
