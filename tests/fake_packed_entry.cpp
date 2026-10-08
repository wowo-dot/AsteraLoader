// An original test-only entry in a .bind-named section. It performs a normal
// protection transition, then enters the stock CRT. No DRM or encryption is
// present. This exercises the production observer without launching the game.
#include <windows.h>

#include <cstdint>

extern "C" void wWinMainCRTStartup();

namespace {
std::uint64_t g_cookie_before = 0;
std::uint8_t g_common_before[22]{};
const std::uint8_t* g_common = nullptr;

const IMAGE_NT_HEADERS64* headers() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
}

std::uint64_t cookie() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto directory = headers()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const auto* config = reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(base + directory.VirtualAddress);
    return *reinterpret_cast<const std::uint64_t*>(config->SecurityCookie);
}
}  // namespace

extern "C" __declspec(dllexport) BOOL cmfm_test_cookie_unchanged() {
    return g_cookie_before != 0 && g_cookie_before != 0x2b992ddfa232ull && cookie() == g_cookie_before;
}

extern "C" __declspec(dllexport) BOOL cmfm_test_common_unchanged() {
    if (!g_common) return FALSE;
    for (unsigned i = 0; i < sizeof(g_common_before); ++i) {
        if (g_common[i] != g_common_before[i]) return FALSE;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL cmfm_test_startup_restored() {
    const auto* entry = reinterpret_cast<const std::uint8_t*>(&wWinMainCRTStartup);
    return entry[0] == 0x48 && entry[1] == 0x83 && entry[2] == 0xec && entry[3] == 0x28 && entry[4] == 0xe8;
}

#pragma code_seg(push, ".bind")
extern "C" void cmfm_test_packed_entry() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* pe = headers();
    g_cookie_before = cookie();
    const auto* entry = reinterpret_cast<const std::uint8_t*>(&wWinMainCRTStartup);
    if (entry[13] != 0xe9) ExitProcess(94);
    const auto delta = *reinterpret_cast<const std::int32_t*>(entry + 14);
    g_common = entry + 18 + delta;
    for (unsigned i = 0; i < sizeof(g_common_before); ++i) g_common_before[i] = g_common[i];
    wchar_t skip[2]{};
    const bool skip_transition = GetEnvironmentVariableW(L"CMFM_FAKE_SKIP_TRANSITION", skip, 2) != 0;
    const auto* section = IMAGE_FIRST_SECTION(pe);
    for (WORD i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        if (skip_transition) break;
        if (section[i].Name[0] == '.' && section[i].Name[1] == 't' && section[i].Name[2] == 'e' &&
            section[i].Name[3] == 'x' && section[i].Name[4] == 't' && section[i].Name[5] == 0) {
            auto* text = const_cast<std::uint8_t*>(base + section[i].VirtualAddress);
            DWORD old = 0, scratch = 0;
            if (!VirtualProtect(text, section[i].Misc.VirtualSize, PAGE_EXECUTE_READWRITE, &old) ||
                !VirtualProtect(text, section[i].Misc.VirtualSize, old, &scratch)) ExitProcess(95);
            break;
        }
    }
    wWinMainCRTStartup();
}
#pragma code_seg(pop)
