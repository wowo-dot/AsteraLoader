// Original fake game, never MHW: al.patch through the production launcher
// and core. The plugin patchy changes a pointer in read-only data, a constant
// in code and a call before PRE_MAIN ends, and a writable table at WIN_MAIN;
// this checks what the game sees.
#include "common/log_file.hpp"
#include "common/windows.hpp"

#include <bit>
#include <cstdint>

extern "C" __declspec(dllexport) const std::uint64_t al_test_menu[2] = {0x3E7A0000000000A1ull, 0x3E7A0000000000B2ull};
extern "C" __declspec(dllexport) std::uint8_t al_test_table[8] = {0x51, 0x62, 0x73, 0x84, 0x95, 0xA6, 0xB7, 0xC8};
extern "C" __declspec(dllexport) __declspec(noinline) int al_test_constant() {
    return 0x1234ABCD;
}
extern "C" __declspec(dllexport) __declspec(noinline) int al_test_callee(int x) {
    return x * 3 + 0x2D2D;
}
extern "C" __declspec(dllexport) __declspec(noinline) int al_test_caller(int x) {
    return al_test_callee(x + 0x1111) * 5 + 0x77;
}

namespace {
int (*volatile g_constant)() = &al_test_constant;
int (*volatile g_caller)(int) = &al_test_caller;
std::uint64_t load64(const volatile std::uint64_t* at) {
    return *at;
}
std::uint8_t load8(const volatile std::uint8_t* at) {
    return *at;
}
}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, char*, int) {
    al::LogFile log;
    if (!log.open(al::environment(L"AL_FAKE_OUTPUT"))) return 90;
    // Made in al_plugin_init: written when PRE_MAIN ended.
    if (load64(&al_test_menu[0]) != 0x3E7A0000000000C3ull || load64(&al_test_menu[1]) != 0x3E7A0000000000B2ull) return 91;
    if (g_constant() != 0x0BADC0DE) return 92;
    if (g_caller(1) != (al_test_callee(1 + 0x1111) + 1000000) * 5 + 0x77) return 93;
    // Made at WIN_MAIN: written at once.
    if (load8(&al_test_table[3]) != 0xEE || load8(&al_test_table[2]) != 0x73) return 94;
    using Toggle = BOOL (*)(BOOL);
    const auto toggle = std::bit_cast<Toggle>(GetProcAddress(GetModuleHandleW(L"patchy.dll"), "al_test_toggle"));
    if (!toggle) return 95;
    if (!toggle(FALSE) || g_constant() != 0x1234ABCD) return 96;
    if (!toggle(TRUE) || g_constant() != 0x0BADC0DE) return 97;
    log.line(L"patch_valid=1");
    return 37;
}
