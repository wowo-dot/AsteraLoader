#include "core/hooks/thread_snapshot.hpp"

#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

DWORD WINAPI worker(void* event) { WaitForSingleObject(event, INFINITE); return 0; }

std::vector<DWORD> process_ids() {
    LPDWORD ids = nullptr;
    UINT count = 0;
    if (!cmfm_enumerate_process_threads(GetProcessHeap(), &ids, &count)) throw 1;
    std::vector<DWORD> result;
    if (count) result.assign(ids, ids + count);
    if (ids) HeapFree(GetProcessHeap(), 0, ids);
    return result;
}

std::vector<DWORD> toolhelp_ids() {
    std::vector<DWORD> result;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw 2;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (!Thread32First(snapshot, &entry)) { CloseHandle(snapshot); throw 3; }
    do {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != GetCurrentThreadId()) result.push_back(entry.th32ThreadID);
        entry.dwSize = sizeof(entry);
    } while (Thread32Next(snapshot, &entry));
    const auto error = GetLastError();
    CloseHandle(snapshot);
    if (error != ERROR_NO_MORE_FILES) throw 4;
    return result;
}

int main() {
    const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return 1;
    std::array<HANDLE, 16> threads{};
    std::array<DWORD, 16> thread_ids{};
    int status = 0;
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i] = CreateThread(nullptr, 0, worker, stop, 0, &thread_ids[i]);
        if (!threads[i]) { status = 2; break; }
    }
    try {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        for (const auto mode : {0, 1}) {
            std::vector<long long> times;
            for (unsigned round = 0; round < 9; ++round) {
                LARGE_INTEGER begin{}, end{};
                QueryPerformanceCounter(&begin);
                const auto ids = mode ? process_ids() : toolhelp_ids();
                QueryPerformanceCounter(&end);
                for (const auto id : thread_ids) if (id && std::find(ids.begin(), ids.end(), id) == ids.end()) throw 5;
                if (std::find(ids.begin(), ids.end(), GetCurrentThreadId()) != ids.end()) throw 6;
                for (const auto id : ids) {
                    const auto handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
                    if (handle) {
                        const auto owner = GetProcessIdOfThread(handle);
                        CloseHandle(handle);
                        if (owner != GetCurrentProcessId()) throw 7;
                    }
                }
                times.push_back((end.QuadPart - begin.QuadPart) * 1000000 / frequency.QuadPart);
            }
            std::sort(times.begin(), times.end());
            std::printf("%s enumeration median_us=%lld min_us=%lld max_us=%lld; 16 owned workers present, caller excluded\n",
                mode ? "process_snapshot" : "system_toolhelp", times[4], times.front(), times.back());
        }
    } catch (...) { status = 3; }
    SetEvent(stop);
    for (const auto thread : threads) if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    CloseHandle(stop);
    return status;
}
