#include "core/crash/handler.hpp"

#include "core/crash/report.hpp"

#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

// Everything on the crash path (unhandled, first_chance and what they call)
// uses static buffers: no heap, no locks, no loader calls. The heap may be
// what broke, and the crashing thread may hold any lock.

namespace cmfm::crash {
namespace {
using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

constexpr std::size_t kKeepReports = 20;  // per kind (.txt, .dmp), newest first
constexpr int kMaxFrames = 48;

std::atomic<bool> g_installed{false};  // install() ran
std::atomic<bool> g_active{false};     // and put the handler in place
std::atomic<bool> g_entered{false};
std::atomic<bool> g_written{false};  // the report (and notice) of the first crash are on disk
std::atomic<int> g_phase{-1};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous{nullptr};  // the filter CMFM replaced
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_replacer{nullptr};  // one that replaced CMFM later
DumpFn g_dump = nullptr;
Minidump g_minidump = Minidump::off;
const void* g_core = nullptr;
const void* g_game = nullptr;
DWORD g_main_thread = 0;
wchar_t g_folder[1024] = {};
char g_version[160] = {};
char g_exe[80] = {};

// A fixed-size UTF-8 text.
struct Text {
    char data[32768];
    std::size_t size;

    void reset() noexcept { size = 0; }
    void add(std::string_view text) noexcept {
        for (const char c : text) {
            if (size + 1 >= sizeof(data)) return;
            data[size++] = c;
        }
    }
    void add(std::wstring_view text) noexcept {
        if (text.empty() || size + 1 >= sizeof(data)) return;
        const int written = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), data + size,
                                                static_cast<int>(sizeof(data) - size - 1), nullptr, nullptr);
        if (written > 0) size += static_cast<std::size_t>(written);
    }
    void hex(std::uint64_t value, int digits = 1) noexcept {
        char buffer[16];
        int count = 0;
        do {
            buffer[count++] = "0123456789abcdef"[value & 0xf];
            value >>= 4;
        } while (value && count < 16);
        while (count < digits && count < 16) buffer[count++] = '0';
        add("0x");
        while (count) add(std::string_view(&buffer[--count], 1));
    }
    void dec(std::uint64_t value, int digits = 1) noexcept {
        char buffer[20];
        int count = 0;
        do {
            buffer[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value && count < 20);
        while (count < digits && count < 20) buffer[count++] = '0';
        while (count) add(std::string_view(&buffer[--count], 1));
    }
};

// A fixed-size wide path.
struct Path {
    wchar_t data[1200];
    std::size_t size;

    void set(const wchar_t* text) noexcept {
        size = 0;
        add(text);
    }
    void add(const wchar_t* text) noexcept {
        while (*text && size + 1 < std::size(data)) data[size++] = *text++;
        data[size] = 0;
    }
    void dec(std::uint64_t value, int digits) noexcept {
        wchar_t buffer[20];
        int count = 0;
        do {
            buffer[count++] = static_cast<wchar_t>(L'0' + value % 10);
            value /= 10;
        } while (value && count < 20);
        while (count < digits && count < 20) buffer[count++] = L'0';
        while (count && size + 1 < std::size(data)) data[size++] = buffer[--count];
        data[size] = 0;
    }
};

struct Module {
    const void* base;
    Owner owner;
    std::wstring_view file;  // the module's file name, within path
    wchar_t path[600];
    DWORD path_size;
};

Text g_report;
Text g_notice;
Path g_report_path;
Path g_dump_path;
Path g_notice_path;
Module g_fault;
Module g_frame_module;
alignas(16) CONTEXT g_walk;
DWORD64 g_frames[kMaxFrames];
int g_frame_count = 0;

const char* phase_name(int phase) noexcept {
    switch (phase) {
    case CMFM_PHASE_PRE_MAIN: return "pre_main";
    case CMFM_PHASE_WIN_MAIN: return "win_main";
    case CMFM_PHASE_DEVICE_CREATED: return "device_created";
    case CMFM_PHASE_SWAPCHAIN_CREATED: return "swapchain_created";
    case CMFM_PHASE_FIRST_PRESENT: return "first_present";
    case CMFM_PHASE_GAME_READY: return "game_ready";
    case CMFM_PHASE_SHUTDOWN: return "shutdown";
    default: return "before PRE_MAIN";
    }
}

// What the user reads: when in the game's life it happened.
const char* phase_moment(int phase) noexcept {
    switch (phase) {
    case CMFM_PHASE_PRE_MAIN:
    case CMFM_PHASE_WIN_MAIN: return "while the game was starting";
    case CMFM_PHASE_DEVICE_CREATED:
    case CMFM_PHASE_SWAPCHAIN_CREATED: return "while the game set up its graphics";
    case CMFM_PHASE_FIRST_PRESENT:
    case CMFM_PHASE_GAME_READY: return "while the game was running";
    case CMFM_PHASE_SHUTDOWN: return "while the game was exiting";
    default: return "early in start-up";
    }
}

bool module_at(const void* address, Module& out) noexcept {
    out.base = nullptr;
    out.owner = {};
    out.file = {};
    out.path_size = 0;
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.Type != MEM_IMAGE) return false;
    out.base = info.AllocationBase;
    // The mapped file's name needs no loader lock, unlike GetModuleFileName.
    out.path_size = K32GetMappedFileNameW(GetCurrentProcess(), info.AllocationBase, out.path,
                                          static_cast<DWORD>(std::size(out.path)));
    const std::wstring_view path(out.path, out.path_size);
    out.owner = classify(path, out.base == g_core, out.base == g_game);
    const auto slash = path.find_last_of(L"\\/");
    out.file = slash == std::wstring_view::npos ? path : path.substr(slash + 1);
    return true;
}

void add_location(Text& text, const void* address, Module& module) noexcept {
    if (module_at(address, module)) {
        text.add(module.file);
        text.add("+");
        text.hex(static_cast<std::uint64_t>(static_cast<const char*>(address) - static_cast<const char*>(module.base)));
    } else {
        text.hex(reinterpret_cast<std::uint64_t>(address));
    }
}

void add_owner(Text& text, const Owner& owner) noexcept {
    text.add(owner_label(owner.kind));
    if (owner.kind == OwnerKind::plugin || owner.kind == OwnerKind::stracker_plugin || owner.kind == OwnerKind::other ||
        owner.kind == OwnerKind::windows) {
        text.add(" ");
        text.add(owner.name);
    }
}

// The plugin-facing description of an owner for the notice.
void add_owner_phrase(Text& text, const Owner& owner) noexcept {
    switch (owner.kind) {
    case OwnerKind::plugin: text.add("the CMFM plugin \""); text.add(owner.name); text.add("\""); break;
    case OwnerKind::stracker_plugin: text.add("the Stracker plugin "); text.add(owner.name); break;
    case OwnerKind::core: text.add("CMFM itself"); break;
    case OwnerKind::game: text.add("the game's own code"); break;
    default: text.add(owner.name.empty() ? std::wstring_view(L"unknown code") : owner.name); break;
    }
}

void walk(const CONTEXT& start) noexcept {
    g_walk = start;
    g_frame_count = 0;
    __try {
        while (g_frame_count < kMaxFrames && g_walk.Rip) {
            const DWORD64 rip = g_walk.Rip, rsp = g_walk.Rsp;
            g_frames[g_frame_count++] = rip;
            DWORD64 image = 0;
            if (const auto* entry = RtlLookupFunctionEntry(rip, &image, nullptr)) {
                void* handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, rip, const_cast<PRUNTIME_FUNCTION>(entry), &g_walk,
                                 &handler_data, &frame, nullptr);
            } else {
                g_walk.Rip = *reinterpret_cast<const DWORD64*>(rsp);  // a leaf function
                g_walk.Rsp = rsp + 8;
            }
            if (g_walk.Rip == rip && g_walk.Rsp == rsp) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A broken stack: keep the frames read so far.
    }
}

HANDLE create(const Path& path, DWORD disposition, DWORD access) noexcept {
    return CreateFileW(path.data, access, FILE_SHARE_READ, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void save(const Path& path, const Text& text, DWORD disposition, DWORD access) noexcept {
    const HANDLE file = create(path, disposition, access);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data, static_cast<DWORD>(text.size), &written, nullptr);
    CloseHandle(file);
}

void write(EXCEPTION_POINTERS* info) noexcept {
    const auto* record = info->ExceptionRecord;
    const auto code = record->ExceptionCode;
    const void* address = record->ExceptionAddress;
    const DWORD thread = GetCurrentThreadId();
    const int phase = g_phase.load();
    SYSTEMTIME now{};
    GetLocalTime(&now);

    g_report_path.set(g_folder);
    g_report_path.add(L"\\crash-");
    g_report_path.dec(now.wYear, 4);
    g_report_path.dec(now.wMonth, 2);
    g_report_path.dec(now.wDay, 2);
    g_report_path.add(L"-");
    g_report_path.dec(now.wHour, 2);
    g_report_path.dec(now.wMinute, 2);
    g_report_path.dec(now.wSecond, 2);
    g_report_path.add(L"-");
    g_report_path.dec(GetCurrentProcessId(), 1);
    g_dump_path = g_report_path;
    g_report_path.add(L".txt");
    g_dump_path.add(L".dmp");

    // The faulting module, the stack, who owns it and who started the thread.
    auto& text = g_report;
    text.reset();
    module_at(address, g_fault);
    const Owner fault = g_fault.owner;
    if (code != 0xC00000FDu) walk(*info->ContextRecord);  // little stack left after an overflow
    Owner owner = fault;
    bool from_stack = false;
    if (!removable(fault.kind)) {
        for (int k = 1; k < g_frame_count; ++k) {
            if (module_at(reinterpret_cast<const void*>(g_frames[k]), g_frame_module) &&
                removable(g_frame_module.owner.kind)) {
                owner = g_frame_module.owner;
                from_stack = true;
                break;
            }
        }
    }
    // Copy the owner's name: g_frame_module is reused below.
    static wchar_t owner_name[260];
    const auto owner_size = std::min<std::size_t>(owner.name.size(), std::size(owner_name) - 1);
    std::copy_n(owner.name.data(), owner_size, owner_name);
    owner.name = std::wstring_view(owner_name, owner_size);

    text.add("CMFM crash report\r\ncmfm=");
    text.add(g_version);
    text.add(" exe_sha256=");
    text.add(g_exe[0] ? std::string_view(g_exe) : std::string_view("unknown"));
    text.add("\r\ntime=");
    text.dec(now.wYear, 4);
    text.add("-");
    text.dec(now.wMonth, 2);
    text.add("-");
    text.dec(now.wDay, 2);
    text.add(" ");
    text.dec(now.wHour, 2);
    text.add(":");
    text.dec(now.wMinute, 2);
    text.add(":");
    text.dec(now.wSecond, 2);
    text.add(".");
    text.dec(now.wMilliseconds, 3);
    text.add(" pid=");
    text.dec(GetCurrentProcessId());
    text.add(" thread=");
    text.dec(thread);
    text.add(thread == g_main_thread ? " (the game's main thread)" : "");
    text.add("\r\nphase=");
    text.add(phase_name(phase));
    text.add(" (");
    text.add(phase_moment(phase));
    text.add(")\r\nexception=");
    text.hex(code, 8);
    text.add(" ");
    text.add(exception_name(code));
    if (code == 0xC0000005u && record->NumberParameters >= 2) {
        const auto kind = record->ExceptionInformation[0];
        text.add(kind == 0 ? ", reading " : kind == 1 ? ", writing " : ", executing ");
        text.hex(record->ExceptionInformation[1]);
    }
    text.add("\r\nat=");
    add_location(text, address, g_fault);
    text.add(" (");
    add_owner(text, fault);
    text.add(")\r\nowner=");
    add_owner(text, owner);
    text.add(removable(owner.kind) ? (from_stack ? " (the first plugin on the stack)" : " (the faulting module)")
                                   : " (no plugin involved)");
    // The thread's origin: the deepest frame outside Windows.
    text.add("\r\nthread_started_in=");
    bool origin = false;
    for (int k = g_frame_count - 1; k >= 0 && !origin; --k) {
        if (module_at(reinterpret_cast<const void*>(g_frames[k]), g_frame_module) &&
            g_frame_module.owner.kind != OwnerKind::windows) {
            add_location(text, reinterpret_cast<const void*>(g_frames[k]), g_frame_module);
            text.add(" (");
            add_owner(text, g_frame_module.owner);
            text.add(")");
            origin = true;
        }
    }
    if (!origin) text.add("unknown");
    text.add("\r\nstack:\r\n");
    for (int k = 0; k < g_frame_count; ++k) {
        text.add("  ");
        text.dec(static_cast<std::uint64_t>(k), 2);
        text.add(" ");
        add_location(text, reinterpret_cast<const void*>(g_frames[k]), g_frame_module);
        text.add(" (");
        add_owner(text, g_frame_module.owner);
        text.add(")\r\n");
    }
    const auto& c = *info->ContextRecord;
    const struct {
        const char* name;
        DWORD64 value;
    } registers[] = {{"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi},
                     {"rdi", c.Rdi}, {"rbp", c.Rbp}, {"rsp", c.Rsp}, {"r8", c.R8},   {"r9", c.R9},
                     {"r10", c.R10}, {"r11", c.R11}, {"r12", c.R12}, {"r13", c.R13}, {"r14", c.R14},
                     {"r15", c.R15}, {"rip", c.Rip}};
    text.add("registers:");
    for (const auto& r : registers) {
        text.add(" ");
        text.add(r.name);
        text.add("=");
        text.hex(r.value, 16);
    }
    text.add("\r\n");
    save(g_report_path, text, CREATE_ALWAYS, GENERIC_WRITE);

    // The notice for the next launch; a crash while exiting gets none.
    if (phase != CMFM_PHASE_SHUTDOWN) {
        auto& notice = g_notice;
        notice.reset();
        notice.add("The game crashed in your last session, ");
        notice.add(phase_moment(phase));
        notice.add(", in ");
        add_owner_phrase(notice, owner);
        if (from_stack) {
            notice.add(" (inside ");
            notice.add(g_fault.file.empty() ? std::wstring_view(L"unknown code") : g_fault.file);
            notice.add(", called from it)");
        }
        notice.add(": ");
        notice.add(exception_name(code));
        notice.add(" at ");
        add_location(notice, address, g_fault);
        notice.add(".");
        if (removable(owner.kind)) notice.add(" If it happens again, try the game without that plugin.");
        notice.add(" Report: ");
        notice.add(std::wstring_view(g_report_path.data, g_report_path.size));
        g_notice_path.set(g_folder);
        g_notice_path.add(L"\\last-crash.txt");
        save(g_notice_path, notice, CREATE_ALWAYS, GENERIC_WRITE);
    }

    // Last, as it is the step most likely to fail: the optional minidump.
    if (g_minidump != Minidump::off && g_dump && code != 0xC00000FDu) {
        const HANDLE file = create(g_dump_path, CREATE_ALWAYS, GENERIC_WRITE);
        bool ok = false;
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{thread, info, FALSE};
            const auto type = g_minidump == Minidump::full
                                  ? static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                                               MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
                                  : static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                               MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            ok = g_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, nullptr, nullptr) != FALSE;
            CloseHandle(file);
        }
        text.reset();
        text.add("minidump=");
        if (ok) {
            text.add(std::wstring_view(g_dump_path.data, g_dump_path.size));
        } else {
            text.add("failed");
            DeleteFileW(g_dump_path.data);
        }
        text.add("\r\n");
        save(g_report_path, text, OPEN_EXISTING, FILE_APPEND_DATA);
    }
}

void write_guarded(EXCEPTION_POINTERS* info) noexcept {
    __try {
        write(info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The report is best effort; the crash still goes on to the next filter.
    }
}

LONG WINAPI unhandled(EXCEPTION_POINTERS* info) {
    // Entered again: a filter that replaced CMFM's and keeps its predecessor
    // calls back into it. Go on to the filter CMFM replaced.
    if (g_entered.exchange(true)) {
        const auto previous = g_previous.load();
        return previous ? previous(info) : EXCEPTION_CONTINUE_SEARCH;
    }
    if (info && info->ExceptionRecord && info->ContextRecord) write_guarded(info);
    g_written = true;
    const auto replacer = g_replacer.load();
    const auto next = replacer ? replacer : g_previous.load();
    const LONG result = next ? next(info) : EXCEPTION_CONTINUE_SEARCH;
    if (result == EXCEPTION_CONTINUE_EXECUTION) g_entered = false;  // someone repaired it: stay armed
    return result;
}

// First chance, for fatal codes only: if something replaced CMFM's
// top-level filter, put CMFM's back in front and remember the other one to
// call after it. Never handles anything.
LONG CALLBACK first_chance(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord && fatal(info->ExceptionRecord->ExceptionCode) && !g_entered.load()) {
        const auto top = SetUnhandledExceptionFilter(&unhandled);
        if (top && top != &unhandled) g_replacer = top;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

bool read_small(const std::wstring& path, std::string& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (1 << 16);
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(file);
    return ok;
}

// Keeps the newest kKeepReports files matching `pattern` (names sort by time).
void prune(const std::wstring& folder, const wchar_t* pattern) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((folder + L"\\" + pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    if (names.size() <= kKeepReports) return;
    std::sort(names.begin(), names.end());
    for (std::size_t k = 0; k + kKeepReports < names.size(); ++k) DeleteFileW((folder + L"\\" + names[k]).c_str());
}

template <std::size_t N>
void copy_text(char (&target)[N], std::string_view text) noexcept {
    const auto size = std::min(text.size(), N - 1);
    std::copy_n(text.data(), size, target);
    target[size] = 0;
}
}  // namespace

void install(const Options& options) noexcept {
    if (g_installed.exchange(true)) return;
    try {
        const auto folder = options.logs_folder + L"\\crashes";
        if (folder.size() + 64 >= std::size(g_folder)) return;  // leave room for the file names
        std::copy(folder.begin(), folder.end(), g_folder);
        g_folder[folder.size()] = 0;
        CreateDirectoryW(options.logs_folder.c_str(), nullptr);
        CreateDirectoryW(g_folder, nullptr);
        copy_text(g_version, options.version);
        copy_text(g_exe, options.exe_sha256);
        std::string settings;
        if (!options.settings_file.empty() && read_small(options.settings_file, settings)) {
            g_minidump = minidump_setting(settings);
        }
        if (g_minidump != Minidump::off) {
            if (const HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
                g_dump = reinterpret_cast<DumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
            }
        }
        prune(folder, L"crash-*.txt");
        prune(folder, L"crash-*.dmp");
        HMODULE core = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&install), &core);
        g_core = core;
        g_game = GetModuleHandleW(nullptr);
        g_main_thread = GetCurrentThreadId();
        g_previous = SetUnhandledExceptionFilter(&unhandled);
        AddVectoredExceptionHandler(0, &first_chance);  // after every other handler
        g_active = true;
    } catch (...) {
        // Without a crash handler the game runs as before.
    }
}

bool installed() noexcept {
    return g_active.load();
}

void note_phase(cmfm_phase_t phase) noexcept {
    g_phase = static_cast<int>(phase);
    if (phase != CMFM_PHASE_SHUTDOWN || !g_entered.load()) return;
    // The game still returned from WinMain after a crash on another thread:
    // the crash raced the game's own shutdown and did not end the session.
    // Withdraw the notice and say so in the report. The crashing thread may
    // still be writing; give it up to two seconds.
    for (int k = 0; k < 200 && !g_written.load(); ++k) Sleep(10);
    if (!g_written.load()) return;
    static Path exit_notice;
    exit_notice.set(g_folder);
    exit_notice.add(L"\\last-crash.exit.txt");
    MoveFileExW(g_notice_path.data, exit_notice.data, MOVEFILE_REPLACE_EXISTING);
    static Text line;
    line.reset();
    line.add("afterwards=the game still returned from WinMain: the crash happened while it was exiting; no notice\r\n");
    save(g_report_path, line, OPEN_EXISTING, FILE_APPEND_DATA);
}

std::wstring take_previous(const std::wstring& logs_folder) {
    const auto path = logs_folder + L"\\crashes\\last-crash.txt";
    std::string text;
    if (!read_small(path, text) || text.empty()) return {};
    MoveFileExW(path.c_str(), (logs_folder + L"\\crashes\\last-crash.seen.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    std::wstring wide(text.size(), L'\0');
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                                         static_cast<int>(wide.size()));
    wide.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    return wide;
}

}  // namespace cmfm::crash
