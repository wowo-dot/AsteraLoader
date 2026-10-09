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
//
// 0.3: hooks-mo2-2 found the loop runs while byte [*(exe+0x51C5320)+0x2BE8]
// is 0. To learn who sets it, the probe reads the flag before and after each
// frame, watches it from a thread of its own until it flips (and stops
// there, before the game tears down), lists every instruction in the game's
// code that writes a byte or more at +0x2BE8 with its function's code, and
// names the two function pointers the loop calls.
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
// hooks-mo2-2, exe 421810 only: the frame loop's object and exit flag, and
// the function pointers it calls at its start and after each frame.
constexpr std::uintptr_t kMainRva = 0x51C5320;
constexpr std::uint32_t kExitFlag = 0x2BE8;
constexpr std::uintptr_t kLoopStartSlot = 0x4C97A70;
constexpr std::uintptr_t kAfterFrameSlot = 0x4C97B00;

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

// The exit flag as seen around frames and by the watcher thread.
std::atomic<bool> g_in_frame{false};
std::uint64_t g_flag_before_frame = 0;  // first frame (1-based) that began with the flag set
std::uint64_t g_flag_after_frame = 0;   // first frame that ended with it set
std::int64_t g_flag_after_qpc = 0;
std::atomic<std::int64_t> g_flip_qpc{0};  // when the watcher saw it set
bool g_flip_in_frame = false;
std::uint64_t g_flip_frames = 0;
std::atomic<bool> g_stop_watch{false};
HANDLE g_watcher = nullptr;

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

// The record does not give Main:Update's signature (sMain arrives in rcx).
// Passing on all four integer argument registers and the return value keeps
// the detour transparent whatever the real arity is.
using UpdateFn = std::uintptr_t (*)(void*, void*, void*, void*);

// The frame loop's exit flag, or -1 before the game creates its main object.
int exit_flag() {
    if (!g_known_exe) return -1;
    const auto* main = *reinterpret_cast<const std::uint8_t* const volatile*>(g_image + kMainRva);
    return main ? *reinterpret_cast<const volatile std::uint8_t*>(main + kExitFlag) : -1;
}

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
    if (!g_flag_before_frame && exit_flag() > 0) g_flag_before_frame = call + 1;
    g_in_frame.store(true, std::memory_order_relaxed);
    const auto result = reinterpret_cast<UpdateFn>(g_next)(main, a2, a3, a4);
    g_in_frame.store(false, std::memory_order_relaxed);
    const auto end = qpc();
    g_last_end_qpc.store(end, std::memory_order_relaxed);
    if (!g_flag_after_frame && exit_flag() > 0) {
        g_flag_after_frame = call + 1;
        g_flag_after_qpc = end;
    }
    return result;
}

// Polls the exit flag until it is set, then stops at once: after that the
// game tears down and its objects go away.
DWORD WINAPI watch_flag(void*) {
    while (!g_stop_watch.load(std::memory_order_relaxed)) {
        if (exit_flag() > 0) {
            g_flip_in_frame = g_in_frame.load(std::memory_order_relaxed);
            g_flip_frames = g_calls.load(std::memory_order_relaxed);
            g_flip_qpc.store(qpc(), std::memory_order_release);
            return 0;
        }
        Sleep(1);
    }
    return 0;
}

// The export a pointer leads to, "module!name", or where it is.
void name_pointer(char* out, std::size_t size, const void* target) {
    where(target, out, size);
    HMODULE module = nullptr;
    if (!target || in_exe(target) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(target), &module)) {
        return;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) return;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    const auto wanted = static_cast<DWORD>(static_cast<const std::uint8_t*>(target) - base);
    for (DWORD k = 0; k < exports->NumberOfNames; ++k) {
        if (functions[ordinals[k]] == wanted) {
            const auto length = std::strlen(out);
            std::snprintf(out + length, size - length, " = %s", reinterpret_cast<const char*>(base + names[k]));
            return;
        }
    }
}

// Every instruction that stores a byte or a dword at [reg+0x2BE8], with the
// code around it and its function's start.
void scan_writers() {
    const char* const patterns[] = {"C6 ?? E8 2B 00 00", "C7 ?? E8 2B 00 00", "88 ?? E8 2B 00 00", "89 ?? E8 2B 00 00"};
    unsigned writers = 0;
    for (const auto* pattern : patterns) {
        void* matches[64] = {};
        const auto count = g_scanner->find(g_self, pattern, AL_SCAN_CODE, matches, 64);
        say(AL_LOG_INFO, "writer pattern \"%s\": %u match(es)", pattern, count);
        for (unsigned k = 0; k < count && k < 64; ++k) {
            const auto* at = static_cast<const std::uint8_t*>(matches[k]);
            if ((at[1] & 0xC0) != 0x80 || (at[1] & 7) == 4) continue;  // [reg+disp32] only
            char bytes[3 * 20 + 1] = {};
            for (int b = 0; b < 20; ++b) std::snprintf(bytes + 3 * b, 4, "%02X ", at[b - 4]);
            DWORD64 image = 0;
            const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(at), &image, nullptr);
            const auto* primary = entry ? primary_entry(entry) : nullptr;
            say(AL_LOG_INFO, "writer %u at exe+0x%llx (from 4 bytes before: %s) fragment 0x%lx-0x%lx primary 0x%lx-0x%lx",
                writers, static_cast<unsigned long long>(rva(at)), bytes, entry ? entry->BeginAddress : 0,
                entry ? entry->EndAddress : 0, primary ? primary->BeginAddress : 0, primary ? primary->EndAddress : 0);
            if (++writers > 24 || !entry) continue;
            // The code before and after the store, within its fragment, and the function's start.
            const auto here = static_cast<std::uint32_t>(rva(at));
            char label[32];
            std::snprintf(label, sizeof(label), "writer %u", writers - 1);
            const std::uint32_t begin = entry->BeginAddress, end = entry->EndAddress;
            dump(label, (std::max)(begin, here > 0x200 ? here - 0x200 : 0u), (std::min)(end, here + 0x80));
            const std::uint32_t start = primary->BeginAddress, finish = primary->EndAddress;
            if (start != begin || here - begin > 0x200) {
                std::snprintf(label, sizeof(label), "writer %u start", writers - 1);
                dump(label, start, (std::min)(finish, start + 0x60));
            }
        }
    }
}

void report_flag(std::int64_t shutdown) {
    if (!g_known_exe) return;
    say(AL_LOG_INFO, "exit flag: first set at the start of frame %llu, at the end of frame %llu (%.1f ms before "
        "SHUTDOWN); %llu frames ran", static_cast<unsigned long long>(g_flag_before_frame),
        static_cast<unsigned long long>(g_flag_after_frame),
        g_flag_after_qpc ? ms_between(g_flag_after_qpc, shutdown) : 0.0, static_cast<unsigned long long>(g_calls.load()));
    if (const auto flip = g_flip_qpc.load(std::memory_order_acquire)) {
        say(AL_LOG_INFO, "watcher saw it set %.1f ms before SHUTDOWN, %s, %llu frames started", ms_between(flip, shutdown),
            g_flip_in_frame ? "during a frame" : "between frames", static_cast<unsigned long long>(g_flip_frames));
    } else {
        say(AL_LOG_INFO, "watcher never saw it set");
    }
    for (const auto slot : {kLoopStartSlot, kAfterFrameSlot}) {
        char text[MAX_PATH + 96];
        name_pointer(text, sizeof(text), *reinterpret_cast<void* const*>(g_image + slot));
        say(AL_LOG_INFO, "slot exe+0x%llx -> %s", static_cast<unsigned long long>(slot), text);
    }
    scan_writers();
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
    describe("frame thread start routine", g_update_start);
    report_flag(now);
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void*) {
    if (phase == AL_PHASE_PRE_MAIN) {
        find_and_hook();
        if (g_known_exe) describe("LuaEngine MapClockLocal", g_image + kLuaClockRva);
    } else if (phase == AL_PHASE_WIN_MAIN) {
        g_main_thread = GetCurrentThreadId();
        if (g_known_exe) g_watcher = CreateThread(nullptr, 0, watch_flag, nullptr, 0, nullptr);
    } else if (phase == AL_PHASE_SHUTDOWN) {
        g_stop_watch.store(true, std::memory_order_relaxed);
        if (g_watcher) {
            WaitForSingleObject(g_watcher, 200);
            CloseHandle(g_watcher);
        }
        report();
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t), AL_API_VERSION, "site_probe", "Astera Loader site probe (diagnostic)", "0.3.0",
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
