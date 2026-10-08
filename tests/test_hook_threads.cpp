#include <windows.h>
#include <MinHook.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdio>

namespace {
using Function = int(__cdecl*)(int);
volatile LONG g_bias = 7;
std::atomic<bool> g_stop{false}, g_bad{false};
std::atomic<unsigned> g_ready{0};
std::atomic<unsigned long long> g_calls{0};
std::atomic<bool> g_held_started{false};
Function g_first_original = nullptr, g_second_original = nullptr;

__declspec(noinline) int first(int value) { return value * 7 + g_bias; }
__declspec(noinline) int second(int value) { return value * 9 + g_bias; }
int first_hook(int value) { return g_first_original(value) + 100; }
int second_hook(int value) { return g_second_original(value) + 200; }

DWORD WINAPI worker(void*) {
    // Indirect calls keep the optimizer from treating the targets as constants.
    Function volatile one = first, two = second;
    g_ready.fetch_add(1);
    while (!g_stop.load()) {
        const auto a = one(3), b = two(3);
        if ((a != 28 && a != 128) || (b != 34 && b != 234)) g_bad.store(true);
        g_calls.fetch_add(1);
        SwitchToThread();
    }
    return 0;
}

DWORD WINAPI held_worker(void* stop) {
    g_held_started.store(true);
    WaitForSingleObject(stop, INFINITE);
    return 0;
}

void require(MH_STATUS status) { if (status != MH_OK) throw 1; }
}

int main() {
    std::array<HANDLE, 8> threads{};
    const auto held_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto held = held_stop ? CreateThread(nullptr, 0, held_worker, held_stop, CREATE_SUSPENDED, nullptr) : nullptr;
    if (!held) { if (held_stop) CloseHandle(held_stop); return 1; }
    int outcome = 0;
    try {
        require(MH_Initialize());
        void* first_original = nullptr;
        void* second_original = nullptr;
        require(MH_CreateHook(std::bit_cast<void*>(&first), std::bit_cast<void*>(&first_hook), &first_original));
        require(MH_CreateHook(std::bit_cast<void*>(&second), std::bit_cast<void*>(&second_hook), &second_original));
        g_first_original = std::bit_cast<Function>(first_original);
        g_second_original = std::bit_cast<Function>(second_original);
        for (auto& thread : threads) {
            thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
            if (!thread) throw 2;
        }
        while (g_ready.load() != threads.size()) SwitchToThread();
        for (unsigned round = 0; round < 12; ++round) {
            require(MH_QueueEnableHook(std::bit_cast<void*>(&first)));
            require(MH_QueueEnableHook(std::bit_cast<void*>(&second)));
            require(MH_ApplyQueued());
            Function volatile one = first, two = second;
            if (one(3) != 128 || two(3) != 234) throw 3;
            require(MH_QueueDisableHook(std::bit_cast<void*>(&first)));
            require(MH_QueueDisableHook(std::bit_cast<void*>(&second)));
            require(MH_ApplyQueued());
            if (one(3) != 28 || two(3) != 34) throw 4;
            if (g_held_started.load()) throw 5;
        }
    } catch (...) { outcome = 1; }
    g_stop.store(true);
    const auto suspend_count = ResumeThread(held);
    if (suspend_count != 1) outcome = 1;
    // Cleanup remains bounded even if a broken freeze left an extra suspension.
    if (suspend_count != static_cast<DWORD>(-1)) {
        for (DWORD left = suspend_count; left > 1; --left) ResumeThread(held);
    }
    SetEvent(held_stop);
    WaitForSingleObject(held, 5000);
    CloseHandle(held);
    CloseHandle(held_stop);
    for (const auto thread : threads) if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    if (g_bad.load() || !g_calls.load()) outcome = 1;
    MH_Uninitialize();
    if (!outcome) std::printf("8 workers, %llu calls; 12 paired enable/disable rounds preserved results and an existing suspension\n", g_calls.load());
    return outcome;
}
