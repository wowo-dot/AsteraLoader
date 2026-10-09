// An original test-only entry in a .bind-named section. It performs a normal
// protection transition, then enters the stock CRT. No DRM or encryption is
// present. This exercises the production observer without launching the game.
#include <windows.h>

#include <cstdint>

#ifdef AL_FAKE_PHASES
extern "C" void WinMainCRTStartup();
#define wWinMainCRTStartup WinMainCRTStartup
#else
extern "C" void wWinMainCRTStartup();
#endif

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

bool wait_for_cache() {
    // Test-only synchronization, using plain Win32 before this exe's CRT.
    // Production detours never wait for the hash/cache thread.
    wchar_t path[2048]{};
    auto length = GetEnvironmentVariableW(L"AL_FAKE_CACHE_LOG_FOLDER", path, 2048);
    if (!length) return true;
    if (length > 2000) return false;
    path[length++] = L'\\';
    for (const auto c : L"core.") if (c) path[length++] = c;
    wchar_t digits[16]{};
    unsigned count = 0;
    for (auto pid = GetCurrentProcessId(); pid; pid /= 10) digits[count++] = static_cast<wchar_t>(L'0' + pid % 10);
    while (count) path[length++] = digits[--count];
    for (const auto c : L".log") path[length++] = c;
    const auto until = GetTickCount64() + 5000;
    constexpr char marker[] = "startup_crt_cache_ready";
    while (GetTickCount64() < until) {
        const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            char buffer[8192]{};
            DWORD read = 0;
            const bool ok = ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE;
            CloseHandle(file);
            if (ok) for (DWORD i = 0; i + sizeof(marker) - 1 <= read; ++i) {
                bool match = true;
                for (unsigned j = 0; j < sizeof(marker) - 1; ++j) if (buffer[i + j] != marker[j]) { match = false; break; }
                if (match) return true;
            }
        }
        Sleep(2);
    }
    return false;
}
}  // namespace

extern "C" __declspec(dllexport) BOOL al_test_cookie_unchanged() {
    return g_cookie_before != 0 && g_cookie_before != 0x2b992ddfa232ull && cookie() == g_cookie_before;
}

extern "C" __declspec(dllexport) BOOL al_test_common_unchanged() {
    if (!g_common) return FALSE;
    for (unsigned i = 0; i < sizeof(g_common_before); ++i) {
        if (g_common[i] != g_common_before[i]) return FALSE;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL al_test_startup_restored() {
    const auto* entry = reinterpret_cast<const std::uint8_t*>(&wWinMainCRTStartup);
    return entry[0] == 0x48 && entry[1] == 0x83 && entry[2] == 0xec && entry[3] == 0x28 && entry[4] == 0xe8;
}

// Model the observed transition's caller outside .bind using original test
// code. This helper is in .text and is called before the stand-in's CRT.
__declspec(noinline) BOOL helper_transition(void* text, SIZE_T size) {
    DWORD old = 0, scratch = 0;
    return VirtualProtect(text, size, PAGE_EXECUTE_READWRITE, &old) &&
           VirtualProtect(text, size, old, &scratch);
}

#pragma code_seg(push, ".bind")
extern "C" void al_test_packed_entry() {
    if (!wait_for_cache()) ExitProcess(99);
    const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* pe = headers();
    g_cookie_before = cookie();
    const auto* entry = reinterpret_cast<const std::uint8_t*>(&wWinMainCRTStartup);
    if (entry[13] != 0xe9) ExitProcess(94);
    const auto delta = *reinterpret_cast<const std::int32_t*>(entry + 14);
    g_common = entry + 18 + delta;
    for (unsigned i = 0; i < sizeof(g_common_before); ++i) g_common_before[i] = g_common[i];
    wchar_t skip[2]{};
    const bool skip_transition = GetEnvironmentVariableW(L"AL_FAKE_SKIP_TRANSITION", skip, 2) != 0;
    wchar_t helper[2]{};
    const bool use_helper = GetEnvironmentVariableW(L"AL_FAKE_HELPER_TRANSITION", helper, 2) != 0;
    DWORD invalid_old = 0x12345678;
    if (VirtualProtect(nullptr, 0, PAGE_READONLY, &invalid_old) || invalid_old != 0x12345678) ExitProcess(96);
    const auto invalid_error = GetLastError();
    if (invalid_error == ERROR_SUCCESS) ExitProcess(97);
    // Overflow the diagnostic cap with rejected calls in the timeout case.
    // The game must still start and the caller's output must remain untouched.
    wchar_t overflow[2]{};
    if (skip_transition || GetEnvironmentVariableW(L"AL_FAKE_TRACE_OVERFLOW", overflow, 2) != 0) {
        for (unsigned i = 0; i < 300; ++i) {
            DWORD output = 0x12345678;
            SetLastError(0);
            if (VirtualProtect(nullptr, 0, PAGE_READONLY, &output) || output != 0x12345678 ||
                GetLastError() != invalid_error) ExitProcess(98);
        }
    }
    const auto* section = IMAGE_FIRST_SECTION(pe);
    for (WORD i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
        if (skip_transition) break;
        if (section[i].Name[0] == '.' && section[i].Name[1] == 't' && section[i].Name[2] == 'e' &&
            section[i].Name[3] == 'x' && section[i].Name[4] == 't' && section[i].Name[5] == 0) {
            auto* text = const_cast<std::uint8_t*>(base + section[i].VirtualAddress);
            DWORD old = 0, scratch = 0;
            if (use_helper) {
                if (!helper_transition(text, section[i].Misc.VirtualSize)) ExitProcess(95);
            } else if (!VirtualProtect(text, section[i].Misc.VirtualSize, PAGE_EXECUTE_READWRITE, &old) ||
                       !VirtualProtect(text, section[i].Misc.VirtualSize, old, &scratch)) ExitProcess(95);
            break;
        }
    }
    wWinMainCRTStartup();
}
#pragma code_seg(pop)
