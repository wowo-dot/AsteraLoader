#pragma once

#include <windows.h>

// MinHook owns the returned IDs using its private heap. A failed snapshot
// returns no partial list so its unchanged Toolhelp path can take over.
extern "C" BOOL WINAPI al_enumerate_process_threads(HANDLE heap, LPDWORD* ids, UINT* count) noexcept;

namespace al::hooks {
struct ThreadSnapshotCounts { unsigned attempts; unsigned succeeded; };
ThreadSnapshotCounts thread_snapshot_counts() noexcept;

// The same with the snapshot functions taken from module (kernel32 above).
// If module lacks any of them, FALSE and no list.
BOOL enumerate_threads_from(HMODULE module, HANDLE heap, LPDWORD* ids, UINT* count) noexcept;
}
