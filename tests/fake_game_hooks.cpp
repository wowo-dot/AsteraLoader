// Original fake game, never MHW: production launcher/core, three plugin
// fixtures, late registration and concurrent callers with detour toggling.
#include "game_hook_fixture.hpp"
#include "common/log_file.hpp"
#include "common/windows.hpp"
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <thread>

namespace {
thread_local std::array<unsigned, 16> trace{};
thread_local unsigned used{};
std::atomic<bool> invalid{false}, stop{false};
std::atomic<unsigned> calls{0};
std::atomic<unsigned> overlap{0};
volatile unsigned magic = 23;
void reset() { used = 0; }
bool check(std::initializer_list<unsigned> expected) {
    if (used != expected.size()) return false;
    unsigned at = 0; for (auto token : expected) if (trace[at++] != token) return false;
    return true;
}
std::array<std::uint8_t, 16> patch{};
}
extern "C" __declspec(dllexport) void cmfm_test_record(unsigned token) {
    if (used == trace.size()) { invalid.store(true); return; }
    trace[used++] = token;
}
extern "C" __declspec(dllexport) __declspec(noinline) double cmfm_test_damage(std::uint64_t a, double b, std::uint64_t c,
    double d, std::uint64_t e, double f, std::uint64_t g, double h) {
    cmfm_test_record(100);
    return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + h * 19 + magic;
}
extern "C" __declspec(dllexport) __declspec(noinline) int cmfm_test_second(int value) {
    cmfm_test_record(6); return value * 13 + static_cast<int>(magic);
}
extern "C" __declspec(dllexport) __declspec(noinline) int cmfm_test_later(int a, int b, int c, int d, int e, int f, int g, int h) {
    cmfm_test_record(8); return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + g * 17 + h * 19 + static_cast<int>(magic);
}
extern "C" BOOL cmfm_test_startup_restored();
extern "C" BOOL cmfm_test_cookie_unchanged();
int WINAPI WinMain(HINSTANCE, HINSTANCE, char*, int) {
    cmfm::LogFile log;
    if (!log.open(cmfm::environment(L"CMFM_FAKE_OUTPUT"))) return 90;
    if (!cmfm_test_startup_restored() || !cmfm_test_cookie_unchanged()) return 91;
    reset();
    if (cmfm_test_damage(1, 2.25, 3, -4.5, 5, 6.75, 7, -8.125) != damage_result + 1 || !check({2, 1, 3, 100})) return 92;
    reset();
    if (cmfm_test_second(11) != 11 * 13 + 23 + 5 || !check({5, 4, 6})) return 93;
    std::memcpy(patch.data(), std::bit_cast<void*>(&cmfm_test_damage), patch.size());
    using Toggle = BOOL (*)(BOOL);
    using Register = BOOL (*)();
    const auto toggle = std::bit_cast<Toggle>(GetProcAddress(GetModuleHandleW(L"hook_beta.dll"), "cmfm_test_toggle"));
    const auto add = std::bit_cast<Register>(GetProcAddress(GetModuleHandleW(L"hook_gamma.dll"), "cmfm_test_register_later"));
    if (!toggle || !add) return 94;
    std::thread caller([&] {
        if (!add()) { invalid.store(true); return; }
        reset();
        if (cmfm_test_later(1, 2, 3, 4, 5, 6, 7, 8) != 477 || !check({7, 8})) { invalid.store(true); return; }
        while (!stop.load()) {
            reset();
            const auto result = cmfm_test_damage(1, 2.25, 3, -4.5, 5, 6.75, 7, -8.125);
            if (!((result == damage_result && check({1, 3, 100})) ||
                (result == damage_result + 1 && check({2, 1, 3, 100})))) invalid.store(true);
            calls.fetch_add(1);
        }
    });
    const auto deadline = GetTickCount64() + 5000;
    while (calls.load() < 100 && !invalid.load() && GetTickCount64() < deadline) std::this_thread::yield();
    for (unsigned i = 0; i < 5000 && !invalid.load(); ++i) {
        const auto before = calls.load();
        if (!toggle(FALSE) || !toggle(TRUE)) invalid.store(true);
        if (calls.load() > before) overlap.fetch_add(1);
        if (!(i % 32)) std::this_thread::yield();
    }
    stop.store(true); caller.join();
    if (invalid.load() || calls.load() < 100 || !overlap.load() ||
        std::memcmp(patch.data(), std::bit_cast<void*>(&cmfm_test_damage), patch.size())) return 95;
    reset();
    if (cmfm_test_damage(1, 2.25, 3, -4.5, 5, 6.75, 7, -8.125) != damage_result + 1 || !check({2, 1, 3, 100})) return 96;
    if (!toggle(FALSE)) return 97;
    reset();
    if (cmfm_test_damage(1, 2.25, 3, -4.5, 5, 6.75, 7, -8.125) != damage_result || !check({1, 3, 100}) || !toggle(TRUE)) return 98;
    log.line(L"game_hooks_valid=1 late_join=1 mixed_args=8 existing_patch=1 toggles=5000 caller_calls=" +
        std::to_wstring(calls.load()) + L" overlap=" + std::to_wstring(overlap.load()));
    return 37;
}
