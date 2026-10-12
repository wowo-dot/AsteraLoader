// The plugin patchy, for the al.patch end-to-end test: changes made in
// al_plugin_init (pending, written when PRE_MAIN ends), one at WIN_MAIN
// (written at once), a refusal, and a switch the fake game calls.
#include "al/al_plugin.h"

#include <windows.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const al_api_t* api{};
const al_patches_t* patches{};
al_plugin_t* self{};
HMODULE game{};
std::uint32_t menu{}, constant{}, call{};
void* callee_original{};
int callee(int x) {
    return std::bit_cast<int (*)(int)>(callee_original)(x) + 1000000;
}
constexpr std::uint64_t kSwapped = 0x3E7A0000000000C3ull;
constexpr std::uint32_t kConstant = 0x0BADC0DE;
std::uint64_t menu_bytes{};
std::uint8_t constant_bytes[4]{}, call_bytes[5]{}, entry_bytes[5]{};
std::string constant_pattern;

std::string hex(const void* at, unsigned count) {
    std::string text;
    for (unsigned k = 0; k < count; ++k) {
        char part[4]{};
        std::snprintf(part, sizeof(part), "%02X", static_cast<const unsigned char*>(at)[k]);
        text += (k ? " " : "") + std::string(part);
    }
    return text;
}

al_patch_t change(std::uint32_t kind, const char* name, void* address, std::uint32_t length, const void* expected,
                  const void* replacement) {
    al_patch_t patch{};
    patch.size = sizeof(patch);
    patch.kind = kind;
    patch.name = name;
    patch.length = length;
    patch.address = address;
    patch.expected = expected;
    patch.replacement = replacement;
    return patch;
}

void note(bool ok, const char* text) {
    api->log(self, ok ? AL_LOG_INFO : AL_LOG_ERROR, text);
}

void AL_CALL phase(al_plugin_t*, al_phase_t id, const void*) {
    if (id != AL_PHASE_WIN_MAIN) return;
    const bool written = patches->state(self, menu) == AL_PATCH_ACTIVE &&
                         patches->state(self, constant) == AL_PATCH_ACTIVE &&
                         patches->state(self, call) == AL_PATCH_ACTIVE;
    note(written, written ? "pending_written=1" : "pending_written=0");
    auto* table = reinterpret_cast<std::uint8_t*>(GetProcAddress(game, "al_test_table"));
    const std::uint8_t was = table ? table[3] : 0, now = 0xEE;
    const auto patch = change(AL_PATCH_DATA, "Table entry", table ? table + 3 : nullptr, 1, &was, &now);
    const auto handle = patches->apply(self, &patch);
    const bool at_once = handle && patches->state(self, handle) == AL_PATCH_ACTIVE;
    note(at_once, at_once ? "win_main_written=1" : "win_main_written=0");
}

const al_plugin_info_t info{sizeof(info), AL_API_VERSION, "patchy", "patchy", "0.0.1",
                            AL_PHASE_BIT(AL_PHASE_WIN_MAIN), 0, nullptr, nullptr, nullptr, phase, nullptr};
}  // namespace

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info() {
    return &info;
}

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* table, const al_init_t* init) {
    api = table;
    self = init->self;
    patches = static_cast<const al_patches_t*>(api->get_interface(self, AL_PATCH_INTERFACE, AL_PATCH_VERSION));
    game = GetModuleHandleW(nullptr);
    auto* menu_table = reinterpret_cast<std::uint8_t*>(GetProcAddress(game, "al_test_menu"));
    auto* constant_fn = reinterpret_cast<std::uint8_t*>(GetProcAddress(game, "al_test_constant"));
    auto* caller_fn = reinterpret_cast<std::uint8_t*>(GetProcAddress(game, "al_test_caller"));
    const auto* callee_fn = reinterpret_cast<const std::uint8_t*>(GetProcAddress(game, "al_test_callee"));
    if (!patches || !menu_table || !constant_fn || !caller_fn || !callee_fn) return AL_ERROR;

    // DATA by address: a pointer in read-only data.
    std::memcpy(&menu_bytes, menu_table, 8);
    auto patch = change(AL_PATCH_DATA, "Start menu", menu_table, 8, &menu_bytes, &kSwapped);
    menu = patches->apply(self, &patch);
    // CODE by pattern: the constant after B8.
    constant_pattern = hex(constant_fn, 6);
    std::memcpy(constant_bytes, constant_fn + 1, 4);
    patch = change(AL_PATCH_CODE, "Constant", nullptr, 4, constant_bytes, &kConstant);
    patch.pattern = constant_pattern.c_str();
    patch.offset = 1;
    constant = patches->apply(self, &patch);
    // CALL by address: the call to al_test_callee.
    std::uint8_t* site = nullptr;
    for (int k = 0; k < 48 && !site; ++k) {
        std::int32_t rel = 0;
        std::memcpy(&rel, caller_fn + k + 1, 4);
        if (caller_fn[k] == 0xE8 && caller_fn + k + 5 + rel == callee_fn) site = caller_fn + k;
    }
    if (site) {
        std::memcpy(call_bytes, site, 5);
        patch = change(AL_PATCH_CALL, "Caller", site, 5, call_bytes, nullptr);
        patch.function = std::bit_cast<void*>(&callee);
        patch.original = &callee_original;
        call = patches->apply(self, &patch);
    }
    // Refused: the entry point lies in the .bind section.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(game) + dos->e_lfanew);
    auto* entry = reinterpret_cast<std::uint8_t*>(game) + nt->OptionalHeader.AddressOfEntryPoint;
    std::memcpy(entry_bytes, entry, 5);
    patch = change(AL_PATCH_CODE, "Entry", entry, 5, entry_bytes, entry_bytes);
    const bool refused = patches->apply(self, &patch) == 0;

    const bool pending = menu && constant && call && patches->state(self, menu) == AL_PATCH_PENDING &&
                         patches->state(self, constant) == AL_PATCH_PENDING &&
                         patches->state(self, call) == AL_PATCH_PENDING;
    note(pending && refused, pending && refused ? "init_pending=1 entry_refused=1" : "init_pending=0");
    return pending && refused ? AL_OK : AL_ERROR;
}

extern "C" __declspec(dllexport) BOOL al_test_toggle(BOOL enabled) {
    return patches->set_enabled(self, constant, enabled ? 1u : 0u) == AL_OK;
}
