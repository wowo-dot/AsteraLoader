// site_probe: a throwaway diagnostic plugin, not shipped. It finds the
// game's per-frame function by SharpPluginLoader's "Main:Update" record
// (MIT, Fexty12573/SharpPluginLoader 2.0.0, AddressRecords.json), hooks it
// through al.hooks, and reports at SHUTDOWN what the core needs for its
// frame tick and "game exiting" event: the thread the frame function runs
// on, the call stack down to WinMain with each function's extent and first
// bytes, how many frames ran, and how long before WinMain returned the last
// one came. It changes nothing in the game: the detour only records.
//
// 0.2 looks for the "game exiting" site. hooks-mo2-1 showed frames stop
// 1.5 s before WinMain returns, so it also records:
// - every thread that ends (DllMain's DLL_THREAD_DETACH) with its start
//   routine, so the report shows whether and when the game's main thread
//   leaves its frame loop;
// - the end of each frame, not just its start;
// - the code of that thread's start routine and of Main:Update, as hex, to
//   find the loop's exit condition offline.
#include "al/al_plugin.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
const al_api_t* g_api = nullptr;
al_plugin_t* g_self = nullptr;
const al_scanner_t* g_scanner = nullptr;
const al_hooks_t* g_hooks = nullptr;
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
std::atomic<std::int64_t> g_last_end_qpc{0};
void* g_update_start = nullptr;  // the frame thread's start routine

// NtQueryInformationThread, looked up at init so the probe imports only KERNEL32.
using QueryThread = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
QueryThread g_query_thread = nullptr;
constexpr int kThreadStartAddress = 9;  // ThreadQuerySetWin32StartAddress

// The latest thread exits; written under the loader lock, so nothing but stores.
struct Exit {
    std::atomic<std::int64_t> qpc{0};  // stored last; 0 = slot never written
    DWORD thread = 0;
    void* start = nullptr;
};
constexpr unsigned kExits = 512;
Exit g_exits[kExits];
std::atomic<unsigned> g_exit_count{0};

void say(al_log_level_t level, const char* format, ...) {
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

// "exe+0x.." or "module.dll+0x..".
void where(const void* address, char* out, std::size_t size) {
    if (in_exe(address)) {
        std::snprintf(out, size, "exe+0x%llx", static_cast<unsigned long long>(rva(address)));
        return;
    }
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &module)) {
        GetModuleFileNameA(module, name, MAX_PATH);
    }
    const char* file = std::strrchr(name, '\\');
    std::snprintf(out, size, "%s+0x%llx", file ? file + 1 : name,
                  static_cast<unsigned long long>(static_cast<const std::uint8_t*>(address) -
                                                  reinterpret_cast<const std::uint8_t*>(module)));
}

// "rva=0x.. function=0x..-0x.. (+0x..) bytes=.." for an address in the exe.
void describe(const char* label, const void* address) {
    if (!in_exe(address)) {
        char text[MAX_PATH + 32];
        where(address, text, sizeof(text));
        say(AL_LOG_INFO, "%s %p outside the exe (%s)", label, address, text);
        return;
    }
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &image, nullptr);
    char bytes[3 * 24 + 1] = {};
    const std::uint8_t* start = entry ? g_image + entry->BeginAddress : nullptr;
    for (int k = 0; start && k < 24; ++k) std::snprintf(bytes + 3 * k, 4, "%02X ", start[k]);
    if (entry) {
        say(AL_LOG_INFO, "%s rva=0x%llx function=0x%lx-0x%lx (+0x%llx) first_bytes=%s", label,
            static_cast<unsigned long long>(rva(address)), entry->BeginAddress, entry->EndAddress,
            static_cast<unsigned long long>(rva(address) - entry->BeginAddress), bytes);
    } else {
        say(AL_LOG_INFO, "%s rva=0x%llx (no unwind entry: a leaf or not a function start)", label,
            static_cast<unsigned long long>(rva(address)));
    }
}

void* current_thread_start() {
    void* start = nullptr;
    if (g_query_thread) g_query_thread(GetCurrentThread(), kThreadStartAddress, &start, sizeof(start), nullptr);
    return start;
}

// Follows chained unwind info (UNW_FLAG_CHAININFO) back to the function's
// primary entry; a split function's cold parts point at it.
const RUNTIME_FUNCTION* primary_entry(const RUNTIME_FUNCTION* entry) {
    for (int guard = 0; entry && guard < 16; ++guard) {
        const std::uint8_t* info = g_image + entry->UnwindData;
        if (!((info[0] >> 3) & 0x4)) return entry;
        const unsigned codes = (info[2] + 1u) & ~1u;  // CountOfCodes, padded to even
        entry = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + 2 * codes);
    }
    return entry;
}

// Hex lines of exe code [begin, end), 32 bytes each, at most 0x600 bytes.
void dump(const char* label, std::uint32_t begin, std::uint32_t end) {
    end = (std::min)(end, begin + 0x600u);
    say(AL_LOG_INFO, "%s code exe+0x%x-0x%x", label, begin, end);
    for (std::uint32_t at = begin; at < end; at += 32) {
        char hex[3 * 32 + 1] = {};
        const auto count = (std::min)(32u, end - at);
        for (std::uint32_t k = 0; k < count; ++k) std::snprintf(hex + 3 * k, 4, "%02X ", g_image[at + k]);
        say(AL_LOG_INFO, "code %x: %s", at, hex);
    }
}

// The function holding `address`: its unwind ranges and its code, once per function.
void dump_function(const char* label, const void* address) {
    static std::uint32_t dumped[8];
    static unsigned dumped_count = 0;
    if (!in_exe(address)) return describe(label, address);
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &image, nullptr);
    if (!entry) return describe(label, address);
    const auto* primary = primary_entry(entry);
    say(AL_LOG_INFO, "%s at exe+0x%llx: fragment 0x%lx-0x%lx, primary 0x%lx-0x%lx", label,
        static_cast<unsigned long long>(rva(address)), entry->BeginAddress, entry->EndAddress,
        primary->BeginAddress, primary->EndAddress);
    for (const auto* part : {primary, entry}) {
        bool seen = false;
        for (unsigned k = 0; k < dumped_count; ++k) seen = seen || dumped[k] == part->BeginAddress;
        if (seen || dumped_count == 8) continue;
        dumped[dumped_count++] = part->BeginAddress;
        dump(label, part->BeginAddress, part->EndAddress);
    }
}

// The record does not give Main:Update's signature (sMain arrives in rcx).
// Passing on all four integer argument registers and the return value keeps
// the detour transparent whatever the real arity is.
using UpdateFn = std::uintptr_t (*)(void*, void*, void*, void*);

std::uintptr_t probe_update(void* main, void* a2, void* a3, void* a4) {
    const auto call = g_calls.fetch_add(1, std::memory_order_relaxed);
    const auto now = qpc();
    if (call == 0) {
        g_update_thread = GetCurrentThreadId();
        g_update_start = current_thread_start();
        g_first_qpc.store(now, std::memory_order_relaxed);
        g_first_count = RtlCaptureStackBackTrace(0, 40, g_first_stack, nullptr);
    } else if (GetCurrentThreadId() != g_update_thread) {
        g_other_threads.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_qpc.store(now, std::memory_order_relaxed);
    const auto result = reinterpret_cast<UpdateFn>(g_next)(main, a2, a3, a4);
    g_last_end_qpc.store(qpc(), std::memory_order_relaxed);
    return result;
}

// Thread exits from 3 s before the last frame on, oldest first, at most 80.
void report_exits(std::int64_t shutdown) {
    const auto total = g_exit_count.load();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const auto since = g_last_qpc.load() - 3 * frequency.QuadPart;
    unsigned listed = 0, first = total > kExits ? total - kExits : 0;
    std::int64_t frame_thread_exit = 0;
    for (unsigned k = first; k < total; ++k) {
        const auto& slot = g_exits[k % kExits];
        const auto at = slot.qpc.load(std::memory_order_acquire);
        if (slot.thread == g_update_thread && at) frame_thread_exit = at;
        if (!at || at < since) continue;
        if (++listed > 80) continue;
        char start[MAX_PATH + 32];
        where(slot.start, start, sizeof(start));
        say(AL_LOG_INFO, "thread exit %+.1f ms from SHUTDOWN: thread %lu start %s%s", -ms_between(at, shutdown),
            slot.thread, start, slot.thread == g_update_thread ? "  <- the frame thread" : "");
    }
    say(AL_LOG_INFO, "thread exits: %u in total, %u in the window (%u listed)", total, listed,
        listed > 80 ? 80u : listed);
    if (frame_thread_exit) {
        say(AL_LOG_INFO, "frame thread %lu ended %.1f ms after its last frame ended, %.1f ms before SHUTDOWN",
            g_update_thread, ms_between(g_last_end_qpc.load(), frame_thread_exit), ms_between(frame_thread_exit, shutdown));
    } else {
        say(AL_LOG_INFO, "frame thread %lu had not ended by SHUTDOWN", g_update_thread);
    }
}

void find_and_hook() {
    void* matches[4] = {};
    const auto count = g_scanner->find(g_self, kUpdatePattern, AL_SCAN_CODE, matches, 4);
    say(AL_LOG_INFO, "Main:Update pattern: %u match(es)", count);
    if (count != 1) return;
    g_update = static_cast<std::uint8_t*>(matches[0]) + kUpdateOffset;
    describe("Main:Update", g_update);
    if (!g_hooks) {
        say(AL_LOG_WARNING, "al.hooks is unavailable; Main:Update not hooked");
        return;
    }
    // Address mode: the pattern must match at address - offset, 25 bytes in.
    const al_hook_t hook{sizeof(al_hook_t), 0, "Main:Update (site_probe)", kUpdatePattern, kUpdateOffset, 0,
                           g_update, reinterpret_cast<void*>(&probe_update), &g_next};
    g_handle = g_hooks->hook(g_self, &hook);
}

void report() {
    say(AL_LOG_INFO, "main thread %lu; Main:Update thread %lu; calls %llu; calls on other threads %llu",
        g_main_thread, g_update_thread, static_cast<unsigned long long>(g_calls.load()),
        static_cast<unsigned long long>(g_other_threads.load()));
    if (g_hooks && g_handle) say(AL_LOG_INFO, "hook state %u", g_hooks->state(g_self, g_handle));
    const auto now = qpc();
    if (g_calls.load()) {
        say(AL_LOG_INFO, "frames ran for %.1f s; the last one started %.1f ms and ended %.1f ms before SHUTDOWN",
            ms_between(g_first_qpc.load(), g_last_qpc.load()) / 1000.0, ms_between(g_last_qpc.load(), now),
            ms_between(g_last_end_qpc.load(), now));
    }
    report_exits(now);
    for (USHORT k = 0; k < g_first_count; ++k) {
        char label[32];
        std::snprintf(label, sizeof(label), "stack[%u]", k);
        describe(label, g_first_stack[k]);
    }
    describe("frame thread start routine", g_update_start);
    dump_function("start routine", g_update_start);
    if (g_first_count > 1) dump_function("frame loop", g_first_stack[1]);
    dump_function("Main:Update", g_update);
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void*) {
    if (phase == AL_PHASE_PRE_MAIN) {
        find_and_hook();
        if (g_known_exe) describe("LuaEngine MapClockLocal", g_image + kLuaClockRva);
    } else if (phase == AL_PHASE_WIN_MAIN) {
        g_main_thread = GetCurrentThreadId();
    } else if (phase == AL_PHASE_SHUTDOWN) {
        report();
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t), AL_API_VERSION, "site_probe", "Astera Loader site probe (diagnostic)", "0.2.0",
    AL_PHASE_BIT(AL_PHASE_PRE_MAIN) | AL_PHASE_BIT(AL_PHASE_WIN_MAIN) | AL_PHASE_BIT(AL_PHASE_SHUTDOWN),
    0, nullptr, nullptr, nullptr, on_phase, nullptr,
};
}  // namespace

// Records each thread that ends. The loader lock is held here: only stores.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_THREAD_DETACH) {
        auto& slot = g_exits[g_exit_count.fetch_add(1, std::memory_order_relaxed) % kExits];
        slot.thread = GetCurrentThreadId();
        slot.start = current_thread_start();
        slot.qpc.store(qpc(), std::memory_order_release);
    }
    return TRUE;
}

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) {
    return &kInfo;
}

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api, const al_init_t* init) {
    g_api = api;
    g_self = init->self;
    g_image = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    g_query_thread = reinterpret_cast<QueryThread>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    g_scanner = static_cast<const al_scanner_t*>(
        api->get_interface(init->self, AL_SCANNER_INTERFACE, AL_SCANNER_VERSION));
    g_hooks = static_cast<const al_hooks_t*>(api->get_interface(init->self, AL_HOOKS_INTERFACE, AL_HOOKS_VERSION));
    const auto* game = static_cast<const al_game_t*>(api->get_interface(init->self, AL_GAME_INTERFACE, AL_GAME_VERSION));
    g_known_exe = game && game->exe_sha256 && std::strcmp(game->exe_sha256, kExe) == 0;
    say(AL_LOG_INFO, "site_probe: exe %s, scanner %s, hooks %s", g_known_exe ? "known (421810)" : "unknown",
        g_scanner ? "yes" : "no", g_hooks ? "yes" : "no");
    return g_scanner ? AL_OK : AL_ERROR;
}
