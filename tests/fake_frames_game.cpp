#include "frame_fixture.hpp"
#include "common/log_file.hpp"
#include "common/windows.hpp"
#include <windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <thread>

namespace {
struct Main { volatile unsigned char exit = 0; std::atomic<unsigned> frames{0}; } main_object;
int title_area{};
std::atomic<unsigned> title_calls{0};
struct Record { unsigned token; DWORD thread; std::uint64_t index; bool flag; };
std::array<Record, 64> records{};
std::atomic<unsigned> used{0};
std::atomic<bool> invalid{false};
HANDLE end_reached{}, flag_set{};
DWORD frame_thread{};
bool backup{};
}
extern "C" {
__declspec(dllexport) void* al_test_main_object = &main_object;
__declspec(dllexport) void al_test_frame_record(unsigned token, std::uint64_t index) {
    const auto slot = used.fetch_add(1);
    if (slot >= records.size()) { invalid.store(true); return; }
    records[slot] = {token, GetCurrentThreadId(), index, main_object.exit != 0};
}
__declspec(dllexport) BOOL al_test_end_wait() {
    SetEvent(end_reached);
    return WaitForSingleObject(flag_set, 5000) == WAIT_OBJECT_0;
}
// Stands in for aTitle's start: the title area starts in frame 1, and again
// in frame 2 (a return to the title); GAME_READY must follow frame 1 only.
__declspec(dllexport) __declspec(noinline) std::uintptr_t al_test_title(void* area, void* a2, void* a3, void* a4) {
    if (area != &title_area || a2 != reinterpret_cast<void*>(0x77) || a3 || a4 != reinterpret_cast<void*>(0x99)) invalid.store(true);
    title_calls.fetch_add(1);
    return 0x7171;
}
__declspec(dllexport) __declspec(noinline) std::uintptr_t al_test_frame(void* main, void* a2, void* a3, void* a4) {
    if (main != &main_object || a2 != reinterpret_cast<void*>(0x1234) || a3 != reinterpret_cast<void*>(0x5678) ||
        a4 != reinterpret_cast<void*>(0x9abc) || GetLastError() != 0x111) invalid.store(true);
    const auto index = main_object.frames.fetch_add(1);
    if ((index == 1 || index == 2) &&
        al_test_title(&title_area, reinterpret_cast<void*>(0x77), nullptr, reinterpret_cast<void*>(0x99)) != 0x7171) invalid.store(true);
    al_test_frame_record(Body, index);
    SetLastError(0x222);
    return 0x123456789abcdef0ull;
}
__declspec(dllexport) __declspec(noinline) void al_test_exit_site(void* main) {
    if (main != &main_object || GetLastError() != 0x123) invalid.store(true);
    al_test_frame_record(ExitRoutine, 0);
    SetLastError(0x333);
}
BOOL al_test_startup_restored();
BOOL al_test_cookie_unchanged();
}
int WINAPI WinMain(HINSTANCE, HINSTANCE, char*, int) {
    al::LogFile log;
    if (!log.open(al::environment(L"AL_FAKE_OUTPUT"))) return 90;
    if (!al_test_startup_restored() || !al_test_cookie_unchanged()) return 91;
    backup = !al::environment(L"AL_FAKE_FRAME_BACKUP").empty();
    al::Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr)), quit(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready || !quit) return 92;
    end_reached = ready.get(); flag_set = quit.get();
    const auto probe = GetModuleHandleW(L"frame_probe.dll");
    using Post = BOOL (*)(); using Validate = BOOL (*)(DWORD, DWORD);
    const auto post = std::bit_cast<Post>(GetProcAddress(probe, "al_test_post_initial"));
    const auto validate = std::bit_cast<Validate>(GetProcAddress(probe, "al_test_frame_validate"));
    if (!post || !validate) return 93;
    std::thread poster([&] { if (!post()) invalid.store(true); }); poster.join();
    std::thread game([&] {
        frame_thread = GetCurrentThreadId();
        // The same site runs once on the frame thread before its loop.
        // This must not announce EXITING.
        SetLastError(0x123); al_test_exit_site(&main_object);
        if (GetLastError() != 0x333) invalid.store(true);
        while (!main_object.exit) {
            SetLastError(0x111);
            if (al_test_frame(&main_object, reinterpret_cast<void*>(0x1234), reinterpret_cast<void*>(0x5678),
                reinterpret_cast<void*>(0x9abc)) != 0x123456789abcdef0ull || GetLastError() != 0x222) invalid.store(true);
            if (main_object.frames.load() > 3) { invalid.store(true); break; }
        }
    });
    const auto reached = WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0;
    if (!backup) {
        SetLastError(0x123); al_test_exit_site(&main_object);
        if (GetLastError() != 0x333) invalid.store(true);
    }
    main_object.exit = 1;
    SetEvent(quit.get()); game.join();
    if (!reached || invalid.load() || !validate(frame_thread, backup ? frame_thread : GetCurrentThreadId())) return 94;
    unsigned exits = 0, final_tasks = 0, bodies = 0, readies = 0;
    // The per-frame trace proves the loader is inside the Reflex markers
    // and around plugin detours at order 0. Exiting may interleave on WinMain.
    std::array<unsigned, 8> expected{OuterBegin, Begin, InnerBegin, Body, InnerEnd, End, OuterEnd, 0};
    unsigned phase = 0;
    for (unsigned i = 0; i < used.load(); ++i) {
        const auto& item = records[i];
        if (item.token == Exiting) { ++exits; if (item.flag != backup) return 95; continue; }
        if (item.token == ExitRoutine) continue;
        if (item.thread != frame_thread) return 96;
        if (item.token == TaskInitial) { if (phase != 1 || bodies) return 97; continue; }
        if (item.token == TaskFinal) { ++final_tasks; if (!item.flag || phase != 6) return 98; continue; }
        // After frame 1's END callbacks, before the outer detour returns.
        if (item.token == GameReady) { ++readies; if (phase != 6 || item.index != 1 || bodies != 2) return 101; continue; }
        if (item.token != expected[phase]) return 99;
        if (item.token == Body) ++bodies;
        phase = (phase + 1) % 7;
    }
    if (phase || bodies != 3 || exits != 1 || final_tasks != 1 || readies != 1 || title_calls.load() != 2) return 100;
    log.line(L"frame_valid=1 frames=3 tasks=2 exiting=1 game_ready=1 backup=" + std::to_wstring(backup));
    return 37;
}
