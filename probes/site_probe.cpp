// site_probe: a throwaway diagnostic plugin, not shipped. It finds the
// game's per-frame function by SharpPluginLoader's "Main:Update" record
// (MIT, Fexty12573/SharpPluginLoader 2.0.0, AddressRecords.json), hooks it
// through cmfm.hooks, and reports at SHUTDOWN what the core needs for its
// frame tick and "game exiting" event: the thread the frame function runs
// on, the call stack down to WinMain with each function's extent and first
// bytes, how many frames ran, and how long before WinMain returned the last
// one came. It changes nothing in the game: the detour only records.
#include "cmfm/cmfm_plugin.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
const cmfm_api_t* g_api = nullptr;
cmfm_plugin_t* g_self = nullptr;
const cmfm_scanner_t* g_scanner = nullptr;
const cmfm_hooks_t* g_hooks = nullptr;
std::uint32_t g_handle = 0;
const std::uint8_t* g_image = nullptr;
bool g_known_exe = false;  // the exe the RVA checks were taken from

// SharpPluginLoader's "Main:Update": the pattern sits 25 bytes into the function.
constexpr const char* kUpdatePattern = "48 8B 8B 88 F8 0A 00 48 8B 01 FF 50 48";
constexpr int kUpdateOffset = -25;
// LuaEngine's per-frame hook for comparison (MapClockLocal, 0x140AE7170).
constexpr std::uintptr_t kLuaClockRva = 0xAE7170;
constexpr const char* kExe = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";

void* g_update = nullptr;
void* g_next = nullptr;
std::atomic<std::uint64_t> g_calls{0};
DWORD g_update_thread = 0;
DWORD g_main_thread = 0;
void* g_first_stack[40];
USHORT g_first_count = 0;
std::atomic<std::int64_t> g_first_qpc{0};
std::atomic<std::int64_t> g_last_qpc{0};
std::atomic<std::uint64_t> g_other_threads{0};

void say(cmfm_log_level_t level, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    g_api->log(g_self, level, text);
}

std::int64_t qpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double ms_between(std::int64_t from, std::int64_t to) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? static_cast<double>(to - from) * 1000.0 / static_cast<double>(frequency.QuadPart) : 0;
}

bool in_exe(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof(info)) && info.AllocationBase == g_image;
}

std::uintptr_t rva(const void* address) {
    return static_cast<std::uintptr_t>(static_cast<const std::uint8_t*>(address) - g_image);
}

// "rva=0x.. function=0x..-0x.. (+0x..) bytes=.." for an address in the exe.
void describe(const char* label, const void* address) {
    if (!in_exe(address)) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(address), &module)) {
            GetModuleFileNameA(module, name, MAX_PATH);
        }
        const char* file = std::strrchr(name, '\\');
        say(CMFM_LOG_INFO, "%s %p outside the exe (%s+0x%llx)", label, address, file ? file + 1 : name,
            module ? static_cast<unsigned long long>(static_cast<const std::uint8_t*>(address) -
                                                     reinterpret_cast<const std::uint8_t*>(module))
                   : 0ull);
        return;
    }
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &image, nullptr);
    char bytes[3 * 24 + 1] = {};
    const std::uint8_t* start = entry ? g_image + entry->BeginAddress : nullptr;
    for (int k = 0; start && k < 24; ++k) std::snprintf(bytes + 3 * k, 4, "%02X ", start[k]);
    if (entry) {
        say(CMFM_LOG_INFO, "%s rva=0x%llx function=0x%lx-0x%lx (+0x%llx) first_bytes=%s", label,
            static_cast<unsigned long long>(rva(address)), entry->BeginAddress, entry->EndAddress,
            static_cast<unsigned long long>(rva(address) - entry->BeginAddress), bytes);
    } else {
        say(CMFM_LOG_INFO, "%s rva=0x%llx (no unwind entry: a leaf or not a function start)", label,
            static_cast<unsigned long long>(rva(address)));
    }
}

void probe_update(void* main) {
    const auto call = g_calls.fetch_add(1, std::memory_order_relaxed);
    const auto now = qpc();
    if (call == 0) {
        g_update_thread = GetCurrentThreadId();
        g_first_qpc.store(now, std::memory_order_relaxed);
        g_first_count = RtlCaptureStackBackTrace(0, 40, g_first_stack, nullptr);
    } else if (GetCurrentThreadId() != g_update_thread) {
        g_other_threads.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_qpc.store(now, std::memory_order_relaxed);
    reinterpret_cast<void (*)(void*)>(g_next)(main);
}

void find_and_hook() {
    void* matches[4] = {};
    const auto count = g_scanner->find(g_self, kUpdatePattern, CMFM_SCAN_CODE, matches, 4);
    say(CMFM_LOG_INFO, "Main:Update pattern: %u match(es)", count);
    if (count != 1) return;
    g_update = static_cast<std::uint8_t*>(matches[0]) + kUpdateOffset;
    describe("Main:Update", g_update);
    if (!g_hooks) {
        say(CMFM_LOG_WARNING, "cmfm.hooks is unavailable; Main:Update not hooked");
        return;
    }
    // Address mode: the pattern must match at address - offset, 25 bytes in.
    const cmfm_hook_t hook{sizeof(cmfm_hook_t), 0, "Main:Update (site_probe)", kUpdatePattern, kUpdateOffset, 0,
                           g_update, reinterpret_cast<void*>(&probe_update), &g_next};
    g_handle = g_hooks->hook(g_self, &hook);
}

void report() {
    say(CMFM_LOG_INFO, "main thread %lu; Main:Update thread %lu; calls %llu; calls on other threads %llu",
        g_main_thread, g_update_thread, static_cast<unsigned long long>(g_calls.load()),
        static_cast<unsigned long long>(g_other_threads.load()));
    if (g_hooks && g_handle) say(CMFM_LOG_INFO, "hook state %u", g_hooks->state(g_self, g_handle));
    const auto now = qpc();
    if (g_calls.load()) {
        say(CMFM_LOG_INFO, "frames ran for %.1f s; the last one %.1f ms before SHUTDOWN",
            ms_between(g_first_qpc.load(), g_last_qpc.load()) / 1000.0, ms_between(g_last_qpc.load(), now));
    }
    for (USHORT k = 0; k < g_first_count; ++k) {
        char label[32];
        std::snprintf(label, sizeof(label), "stack[%u]", k);
        describe(label, g_first_stack[k]);
    }
}

void CMFM_CALL on_phase(cmfm_plugin_t*, cmfm_phase_t phase, const void*) {
    if (phase == CMFM_PHASE_PRE_MAIN) {
        find_and_hook();
        if (g_known_exe) describe("LuaEngine MapClockLocal", g_image + kLuaClockRva);
    } else if (phase == CMFM_PHASE_WIN_MAIN) {
        g_main_thread = GetCurrentThreadId();
    } else if (phase == CMFM_PHASE_SHUTDOWN) {
        report();
    }
}

const cmfm_plugin_info_t kInfo = {
    sizeof(cmfm_plugin_info_t), CMFM_API_VERSION, "site_probe", "CMFM site probe (diagnostic)", "0.1.0",
    CMFM_PHASE_BIT(CMFM_PHASE_PRE_MAIN) | CMFM_PHASE_BIT(CMFM_PHASE_WIN_MAIN) | CMFM_PHASE_BIT(CMFM_PHASE_SHUTDOWN),
    0, nullptr, nullptr, nullptr, on_phase, nullptr,
};
}  // namespace

extern "C" CMFM_PLUGIN_EXPORT const cmfm_plugin_info_t* CMFM_CALL cmfm_plugin_info(void) {
    return &kInfo;
}

extern "C" CMFM_PLUGIN_EXPORT cmfm_result_t CMFM_CALL cmfm_plugin_init(const cmfm_api_t* api, const cmfm_init_t* init) {
    g_api = api;
    g_self = init->self;
    g_image = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    g_scanner = static_cast<const cmfm_scanner_t*>(
        api->get_interface(init->self, CMFM_SCANNER_INTERFACE, CMFM_SCANNER_VERSION));
    g_hooks = static_cast<const cmfm_hooks_t*>(api->get_interface(init->self, CMFM_HOOKS_INTERFACE, CMFM_HOOKS_VERSION));
    const auto* game = static_cast<const cmfm_game_t*>(api->get_interface(init->self, CMFM_GAME_INTERFACE, CMFM_GAME_VERSION));
    g_known_exe = game && game->exe_sha256 && std::strcmp(game->exe_sha256, kExe) == 0;
    say(CMFM_LOG_INFO, "site_probe: exe %s, scanner %s, hooks %s", g_known_exe ? "known (421810)" : "unknown",
        g_scanner ? "yes" : "no", g_hooks ? "yes" : "no");
    return g_scanner ? CMFM_OK : CMFM_ERROR;
}
