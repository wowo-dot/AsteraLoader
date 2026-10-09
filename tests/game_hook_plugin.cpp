#include "game_hook_fixture.hpp"
#include "cmfm/cmfm_plugin.h"
#include <windows.h>
#include <MinHook.h>
#include <bit>
#include <cstdio>
#include <string>

namespace {
const cmfm_api_t* api{};
const cmfm_hooks_t* hooks{};
const cmfm_d3d12_t* graphics{};
cmfm_plugin_t* self{};
GameDamage damage{};
GameSecond second{};
GameLater later{};
GameRecord record{};
void *damage_next{}, *second_next{}, *external_next{}, *later_next{};
std::uint32_t damage_handle{};
std::string signature;
std::string bytes(void* target, unsigned count) {
    std::string result;
    for (unsigned i = 0; i < count; ++i) {
        char text[4]{}; std::snprintf(text, sizeof(text), "%02X", static_cast<unsigned char*>(target)[i]);
        if (i) result += ' '; result += text;
    }
    return result;
}
double detour(std::uint64_t a, double b, std::uint64_t c, double d, std::uint64_t e, double f, std::uint64_t g, double h) {
    record(GAME_HOOK_ROLE);
    const auto result = std::bit_cast<GameDamage>(damage_next)(a, b, c, d, e, f, g, h);
    return result + (GAME_HOOK_ROLE == 2 ? 1 : 0);
}
#if GAME_HOOK_ROLE == 1
int external(int value) { record(4); return std::bit_cast<GameSecond>(external_next)(value) + 5; }
int second_detour(int value) { record(5); return std::bit_cast<GameSecond>(second_next)(value); }
#endif
#if GAME_HOOK_ROLE == 3
int later_detour(int a, int b, int c, int d, int e, int f, int g, int h) {
    record(7); return std::bit_cast<GameLater>(later_next)(a, b, c, d, e, f, g, h);
}
#endif
bool register_damage() {
    const cmfm_hook_t hook{sizeof(hook), GAME_HOOK_ROLE == 2 ? -10 : 0, "FixtureDamage", signature.c_str(), 0, 0,
        GAME_HOOK_ROLE == 2 ? std::bit_cast<void*>(damage) : nullptr, std::bit_cast<void*>(&detour), &damage_next};
    damage_handle = hooks->hook(self, &hook);
    return damage_handle != 0;
}
void CMFM_CALL phase(cmfm_plugin_t*, cmfm_phase_t id, const void*) {
#if GAME_HOOK_ROLE == 1
    if (id == CMFM_PHASE_PRE_MAIN) {
        void* target = std::bit_cast<void*>(second);
        if (MH_Initialize() != MH_OK || MH_CreateHook(target, std::bit_cast<void*>(&external), &external_next) != MH_OK ||
            MH_EnableHook(target) != MH_OK) { api->set_status(self, CMFM_STATUS_DISABLED, "existing hook fixture failed"); return; }
        const auto pattern = bytes(target, 32); // Signature includes the existing external patch.
        const cmfm_hook_t hook{sizeof(hook), 0, "FixtureSecond", pattern.c_str(), 0, 0, target,
            std::bit_cast<void*>(&second_detour), &second_next};
        if (!hooks->hook(self, &hook)) api->set_status(self, CMFM_STATUS_DISABLED, "second registration failed");
    }
#elif GAME_HOOK_ROLE == 3
    if (id == CMFM_PHASE_WIN_MAIN) {
        const auto valid = register_damage();
        api->log(self, valid ? CMFM_LOG_INFO : CMFM_LOG_ERROR, valid ? "win_main_join=active" : "win_main_join=failed");
        // cmfm.d3d12's internal region must not bypass game-function chains.
        if (graphics) graphics->begin_internal(self);
        const auto result = damage(1, 2.25, 3, -4.5, 5, 6.75, 7, -8.125);
        if (graphics) graphics->end_internal(self);
        api->log(self, result == damage_result + 1 ? CMFM_LOG_INFO : CMFM_LOG_ERROR,
            result == damage_result + 1 ? "internal_does_not_bypass=1" : "internal_does_not_bypass=0");
    }
#else
    (void)id;
#endif
}
constexpr const char* names[]{"", "hook_alpha", "hook_beta", "hook_gamma"};
const cmfm_plugin_info_t info{sizeof(info), CMFM_API_VERSION, names[GAME_HOOK_ROLE], names[GAME_HOOK_ROLE], "0.0.1",
    CMFM_PHASE_BIT(CMFM_PHASE_PRE_MAIN) | CMFM_PHASE_BIT(CMFM_PHASE_WIN_MAIN), 0,
    nullptr, nullptr, nullptr, phase, nullptr};
}
extern "C" CMFM_PLUGIN_EXPORT const cmfm_plugin_info_t* CMFM_CALL cmfm_plugin_info() { return &info; }
extern "C" CMFM_PLUGIN_EXPORT cmfm_result_t CMFM_CALL cmfm_plugin_init(const cmfm_api_t* table, const cmfm_init_t* init) {
    api = table; self = init->self;
    hooks = static_cast<const cmfm_hooks_t*>(api->get_interface(self, CMFM_HOOKS_INTERFACE, CMFM_HOOKS_VERSION));
    graphics = static_cast<const cmfm_d3d12_t*>(api->get_interface(self, CMFM_D3D12_INTERFACE, CMFM_D3D12_VERSION));
    const auto game = GetModuleHandleW(nullptr);
    damage = std::bit_cast<GameDamage>(GetProcAddress(game, "cmfm_test_damage"));
    second = std::bit_cast<GameSecond>(GetProcAddress(game, "cmfm_test_second"));
    later = std::bit_cast<GameLater>(GetProcAddress(game, "cmfm_test_later"));
    record = std::bit_cast<GameRecord>(GetProcAddress(game, "cmfm_test_record"));
    if (!hooks || !damage || !second || !later || !record) return CMFM_ERROR;
    signature = bytes(std::bit_cast<void*>(damage), 64);
    return GAME_HOOK_ROLE == 3 || register_damage() ? CMFM_OK : CMFM_ERROR;
}
#if GAME_HOOK_ROLE == 2
extern "C" __declspec(dllexport) BOOL cmfm_test_toggle(BOOL enabled) {
    return hooks->set_enabled(self, damage_handle, enabled) == CMFM_OK;
}
#endif
#if GAME_HOOK_ROLE == 3
extern "C" __declspec(dllexport) BOOL cmfm_test_register_later() {
    const auto pattern = bytes(std::bit_cast<void*>(later), 32);
    const cmfm_hook_t hook{sizeof(hook), 0, "FixtureLater", pattern.c_str(), 0, 0, std::bit_cast<void*>(later),
        std::bit_cast<void*>(&later_detour), &later_next};
    return hooks->hook(self, &hook) != 0;
}
#endif
