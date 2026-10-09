#pragma once

#include <windows.h>

// MinHook owns the returned IDs using its private heap. A failed snapshot
// returns no partial list so its unchanged Toolhelp path can take over.
extern "C" BOOL WINAPI al_enumerate_process_threads(HANDLE heap, LPDWORD* ids, UINT* count) noexcept;

namespace al::hooks {
struct ThreadSnapshotCounts { unsigned attempts; unsigned succeeded; };
ThreadSnapshotCounts thread_snapshot_counts() noexcept;
}
