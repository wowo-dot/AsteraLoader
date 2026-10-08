#include "core/hooks/thread_snapshot.hpp"

#include <processsnapshot.h>
#include <atomic>

namespace {
std::atomic<unsigned> g_attempts{0}, g_succeeded{0};
struct Snapshot {
    HPSS value = nullptr;
    ~Snapshot() { if (value) PssFreeSnapshot(GetCurrentProcess(), value); }
};
struct Marker {
    HPSSWALK value = nullptr;
    ~Marker() { if (value) PssWalkMarkerFree(value); }
};
struct Ids {
    HANDLE heap;
    LPDWORD value = nullptr;
    ~Ids() { if (value) HeapFree(heap, 0, value); }
};
}

extern "C" BOOL WINAPI cmfm_enumerate_process_threads(HANDLE heap, LPDWORD* ids, UINT* count) noexcept {
    g_attempts.fetch_add(1);
    *ids = nullptr;
    *count = 0;
    Snapshot snapshot;
    // IDs only: no contexts, virtual memory, handles or clone process captured.
    if (PssCaptureSnapshot(GetCurrentProcess(), PSS_CAPTURE_THREADS, 0, &snapshot.value) != ERROR_SUCCESS) return FALSE;
    PSS_THREAD_INFORMATION info{};
    if (PssQuerySnapshot(snapshot.value, PSS_QUERY_THREAD_INFORMATION, &info, sizeof(info)) != ERROR_SUCCESS) return FALSE;
    Marker marker;
    if (PssWalkMarkerCreate(nullptr, &marker.value) != ERROR_SUCCESS) return FALSE;
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
        const auto status = PssWalkSnapshot(snapshot.value, PSS_WALK_THREADS, marker.value, &thread, sizeof(thread));
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

cmfm::hooks::ThreadSnapshotCounts cmfm::hooks::thread_snapshot_counts() noexcept {
    const auto succeeded = g_succeeded.load();
    return {g_attempts.load(), succeeded};
}
