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
//
// 0.4: astera-mo2-1 found the WinMain thread's message loop sets the flag
// after WM_QUIT, and that a process ended from outside gets no SHUTDOWN. The
// probe now writes data\site_probe\exit-report.<pid>.txt from DllMain when the
// process ends (the stack of the thread ending it), finds every call to the
// function the message loop calls right before setting the flag, dumps it
// and the frame thread's teardown, and times the gaps between frames.
#include "al/al_plugin.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

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

// The same at process exit, when the game may have freed its object: -2 on a fault.
int exit_flag_guarded() {
    __try {
        return exit_flag();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// Time from one frame's end to the next one's start: SteamAPI_RunCallbacks
// and the loop's check, or Sleep(1) while the window is inactive. A flag set
// in this gap is missed by a check after each frame.
constexpr std::int64_t kGapLimitsUs[] = {20, 100, 500, 2000, 10000};
std::uint64_t g_gaps[6] = {};
std::int64_t g_gap_max_us = 0;
std::int64_t g_gap_sum_us = 0;
std::int64_t g_frame_sum_us = 0;
std::int64_t g_qpf = 1;

void count_gap(std::int64_t start) {
    const auto previous = g_last_end_qpc.load(std::memory_order_relaxed);
    if (!previous) return;
    const auto us = (start - previous) * 1000000 / g_qpf;
    unsigned bucket = 0;
    while (bucket < 5 && us >= kGapLimitsUs[bucket]) ++bucket;
    ++g_gaps[bucket];
    g_gap_sum_us += us;
    g_gap_max_us = (std::max)(g_gap_max_us, us);
}

std::uintptr_t probe_update(void* main, void* a2, void* a3, void* a4) {
    const auto call = g_calls.fetch_add(1, std::memory_order_relaxed);
    const auto now = qpc();
    count_gap(now);
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
    g_frame_sum_us += (end - now) * 1000000 / g_qpf;
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

// astera-mo2-1, exe 421810: the WinMain thread's message loop (exe+0x22e4670)
// calls one function right before it sets the exit flag (the call at
// exe+0x22e482f); the frame thread's teardown starts at exe+0x22e4c0c.
constexpr std::uintptr_t kMessageLoopRva = 0x22e4670;
constexpr std::uintptr_t kBeforeFlagCallRva = 0x22e482f;
constexpr std::uintptr_t kTeardownRva = 0x22e4c0c;

// Call and jump sites (E8/E9 rel32) in the exe's executable sections that
// lead to `target`. Fills `sites`, returns how many there are; 0xFFFFFFFF on a fault.
unsigned find_callers(std::uint32_t target, std::uint32_t* sites, unsigned capacity) {
    unsigned found = 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_image);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_image + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
            if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::uint8_t* code = g_image + section->VirtualAddress;
            const std::uint32_t size = section->Misc.VirtualSize;
            for (std::uint32_t k = 0; k + 5 <= size; ++k) {
                if (code[k] != 0xE8 && code[k] != 0xE9) continue;
                std::int32_t relative;
                std::memcpy(&relative, code + k + 1, 4);
                const auto from = section->VirtualAddress + k;
                if (static_cast<std::uint32_t>(from + 5 + relative) != target) continue;
                if (found < capacity) sites[found] = from;
                ++found;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
    return found;
}

void report_callers(const char* label, std::uint32_t target) {
    std::uint32_t sites[16];
    const auto count = find_callers(target, sites, 16);
    if (count == 0xFFFFFFFFu) {
        say(AL_LOG_INFO, "%s exe+0x%x: the code scan faulted", label, target);
        return;
    }
    say(AL_LOG_INFO, "%s exe+0x%x: %u call or jump site(s)", label, target, count);
    for (unsigned k = 0; k < count && k < 16; ++k) {
        DWORD64 image = 0;
        const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(g_image + sites[k]), &image, nullptr);
        const auto* primary = entry ? primary_entry(entry) : nullptr;
        say(AL_LOG_INFO, "  %s at exe+0x%x in function 0x%lx", g_image[sites[k]] == 0xE8 ? "call" : "jump", sites[k],
            primary ? primary->BeginAddress : 0);
    }
}

// The places a "game exiting" event could hang on, read at WIN_MAIN so a
// launch that never reaches SHUTDOWN still reports them.
void report_exit_sites() {
    if (!g_known_exe) return;
    const auto* call = g_image + kBeforeFlagCallRva;
    if (call[0] != 0xE8) {
        say(AL_LOG_INFO, "no call at exe+0x%llx", static_cast<unsigned long long>(kBeforeFlagCallRva));
        return;
    }
    std::int32_t relative;
    std::memcpy(&relative, call + 1, 4);
    const auto callee = static_cast<std::uint32_t>(kBeforeFlagCallRva + 5 + relative);
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(g_image + callee), &image, nullptr);
    say(AL_LOG_INFO, "called before the flag is set: exe+0x%x, function 0x%lx-0x%lx", callee,
        entry ? entry->BeginAddress : 0, entry ? entry->EndAddress : 0);
    dump("before-flag callee", callee, entry ? (std::min)(static_cast<std::uint32_t>(entry->EndAddress), callee + 0x180)
                                             : callee + 0x80);
    report_callers("callers of the before-flag callee", callee);
    report_callers("callers of the message loop", static_cast<std::uint32_t>(kMessageLoopRva));
    dump("frame thread teardown", static_cast<std::uint32_t>(kTeardownRva), static_cast<std::uint32_t>(kTeardownRva + 0x200));
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
    const auto gaps = g_gaps[0] + g_gaps[1] + g_gaps[2] + g_gaps[3] + g_gaps[4] + g_gaps[5];
    if (gaps) {
        say(AL_LOG_INFO, "between frames: mean %.1f us, max %lld us; <20us %llu, <100us %llu, <500us %llu, <2ms %llu, "
            "<10ms %llu, more %llu; frames take %.1f us on average",
            static_cast<double>(g_gap_sum_us) / static_cast<double>(gaps), g_gap_max_us,
            static_cast<unsigned long long>(g_gaps[0]), static_cast<unsigned long long>(g_gaps[1]),
            static_cast<unsigned long long>(g_gaps[2]), static_cast<unsigned long long>(g_gaps[3]),
            static_cast<unsigned long long>(g_gaps[4]), static_cast<unsigned long long>(g_gaps[5]),
            static_cast<double>(g_frame_sum_us) / static_cast<double>(g_calls.load()));
    }
    for (const auto slot : {kLoopStartSlot, kAfterFrameSlot}) {
        char text[MAX_PATH + 96];
        name_pointer(text, sizeof(text), *reinterpret_cast<void* const*>(g_image + slot));
        say(AL_LOG_INFO, "slot exe+0x%llx -> %s", static_cast<unsigned long long>(slot), text);
    }
}

// Written from DllMain when the process ends, with or without SHUTDOWN: who
// ended it (the calling thread's stack) and the state of the frame loop.
wchar_t g_exit_report[MAX_PATH * 2] = {};
std::atomic<bool> g_shutdown_seen{false};

void write_exit_report() {
    if (!g_exit_report[0]) return;
    static char text[16384];
    std::size_t used = 0;
    const auto add = [&](const char* format, auto... values) {
        if (used < sizeof(text)) {
            const auto n = std::snprintf(text + used, sizeof(text) - used, format, values...);
            if (n > 0) used = (std::min)(sizeof(text), used + static_cast<std::size_t>(n));
        }
    };
    const auto now = qpc();
    const auto thread = GetCurrentThreadId();
    add("process exit on thread %lu (%s); SHUTDOWN %s\r\n", thread,
        thread == g_main_thread ? "the WinMain thread" : thread == g_update_thread ? "the frame thread" : "another thread",
        g_shutdown_seen.load() ? "was dispatched" : "never came");
    add("frames %llu, the last ended %.1f ms ago; exit flag now %d, first seen after frame %llu\r\n",
        static_cast<unsigned long long>(g_calls.load()), ms_between(g_last_end_qpc.load(), now),
        g_shutdown_seen.load() ? -3 : exit_flag_guarded(), static_cast<unsigned long long>(g_flag_after_frame));
    void* stack[48];
    const auto count = RtlCaptureStackBackTrace(0, 48, stack, nullptr);
    for (USHORT k = 0; k < count; ++k) {
        char place[MAX_PATH + 32];
        where(stack[k], place, sizeof(place));
        add("  %02u %s\r\n", static_cast<unsigned>(k), place);
    }
    const auto file = CreateFileW(g_exit_report, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(used), &written, nullptr);
    CloseHandle(file);
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
        report_exit_sites();
    } else if (phase == AL_PHASE_SHUTDOWN) {
        g_shutdown_seen.store(true);
        g_stop_watch.store(true, std::memory_order_relaxed);
        if (g_watcher) {
            WaitForSingleObject(g_watcher, 200);
            CloseHandle(g_watcher);
        }
        report();
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t), AL_API_VERSION, "site_probe", "Astera Loader site probe (diagnostic)", "0.4.0",
    AL_PHASE_BIT(AL_PHASE_PRE_MAIN) | AL_PHASE_BIT(AL_PHASE_WIN_MAIN) | AL_PHASE_BIT(AL_PHASE_SHUTDOWN),
    0, nullptr, nullptr, nullptr, on_phase, nullptr,
};
}  // namespace

// Records each thread that ends. The loader lock is held here: only stores.
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved) write_exit_report();  // the process is ending
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
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_qpf = frequency.QuadPart ? frequency.QuadPart : 1;
    if (init->size >= offsetof(al_init_t, data_folder) + sizeof(init->data_folder) && init->data_folder) {
        // One report per launch: exit-report.<pid>.txt.
        std::swprintf(g_exit_report, std::size(g_exit_report), L"%ls\\exit-report.%lu.txt", init->data_folder,
                      GetCurrentProcessId());
    }
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
