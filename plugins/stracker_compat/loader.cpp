// loader.dll: CMFM's stand-in for Stracker's Loader's loader.dll.
//
// It exports Stracker's ABI (loader_abi.h), so a Stracker plugin's loader.dll
// import binds to it, and runs Stracker's start-up (MHWLoader/loaderdll.cpp,
// config.cpp and log.cpp of Stracker's Loader, MIT, Copyright (c) 2020 Remi
// Lavergne): read loader-config.json, warn about obsolete files, find the
// game version, load nativePC\plugins. Log texts and loader.log / console
// output follow Stracker. Differences, on purpose:
// - paths are taken from the game folder CMFM passes in, not the current
//   directory;
// - a broken config, an unknown logLevel or an unreadable version pointer is
//   logged and reported to CMFM instead of throwing or crashing;
// - every log line also goes to CMFM (stracker_compat's log).

#define CMFM_STRACKER_SHIM
#include "loader_abi.h"
#include "shim.h"
#include "stracker_logic.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

const char* loader::GameVersion = "???";
loader::LogLevel loader::MinLogLevel = loader::INFO;

namespace {
using namespace cmfm::stracker;

// Plugins may log from any thread; the lock guards the outputs below.
SRWLOCK g_lock = SRWLOCK_INIT;
Config g_config;
std::wstring g_log_path;
cmfm_stracker_log_fn g_sink = nullptr;
void* g_sink_context = nullptr;
HANDLE g_console = nullptr;
std::string g_version;
volatile LONG g_started = 0;

class Lock {
public:
    Lock() { AcquireSRWLockExclusive(&g_lock); }
    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

// loader.log, appended like Stracker's text-mode ofstream: "[ HH:MM:SS ] text".
void to_file(const char* stamp, const std::string& text) {
    if (!g_config.logfile || g_log_path.empty()) return;
    const HANDLE file = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string line = std::string("[ ") + stamp + " ] ";
    for (const char c : text) {
        if (c == '\n') line += '\r';
        line += c;
    }
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

// Stracker's "logcmd" console window.
void to_console(int level, const char* stamp, const std::string& text) {
    if (!g_config.logcmd) return;
    if (!g_console) {
        AllocConsole();
        SetConsoleTitleA("Stracker's Loader");
        g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    const auto write = [](const char* data, std::size_t size) {
        WriteConsoleA(g_console, data, static_cast<DWORD>(size), nullptr, nullptr);
    };
    constexpr WORD kWhite = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
    SetConsoleTextAttribute(g_console, FOREGROUND_GREEN);
    write("[ ", 2);
    write(stamp, std::strlen(stamp));
    write(" ] ", 3);
    SetConsoleTextAttribute(g_console, level == loader::ERR    ? FOREGROUND_RED | FOREGROUND_INTENSITY
                                       : level == loader::WARN ? kWhite | FOREGROUND_INTENSITY
                                                               : kWhite);
    write(text.data(), text.size());
    SetConsoleTextAttribute(g_console, kWhite);
}

// Stracker's _log: level filter, time stamp, file and console; plus CMFM.
void write_line(int level, const std::string& text) noexcept {
    if (level < loader::MinLogLevel) return;
    try {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
        Lock lock;
        to_file(stamp, text);
        to_console(level, stamp, text);
        if (g_sink) {
            auto line = text;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            g_sink(g_sink_context, level, line.c_str());
        }
    } catch (...) { /* Logging never throws into a plugin. */ }
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                            nullptr);
    }
    return out;
}

void copy_text(char* destination, std::size_t capacity, const std::string& text) {
    const auto length = std::min(text.size(), capacity - 1);
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool read_file(const std::wstring& path, std::string& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= (1 << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
                             read == out.size());
    }
    CloseHandle(file);
    return ok;
}

// Committed, readable memory: the scan and the version pointer may only touch that.
bool readable(const void* address, std::size_t size) {
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    const auto end = cursor + size;
    if (end < cursor) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info))) return false;
        const auto access = info.Protect & 0xFF;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || access == PAGE_NOACCESS ||
            access == PAGE_EXECUTE) {
            return false;
        }
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

void load_config(const std::wstring& game, cmfm_stracker_result& result) {
    std::string text;
    std::vector<std::string> problems;
    if (!read_file(game + L"\\loader-config.json", text)) {
        result.config_state = CMFM_STRACKER_CONFIG_MISSING;
        g_config = {};
    } else if (!parse_config(text, g_config, problems)) {
        result.config_state = CMFM_STRACKER_CONFIG_INVALID;
        g_config = {};
    } else {
        result.config_state = CMFM_STRACKER_CONFIG_FOUND;
    }
    result.config_problems = static_cast<std::uint32_t>(problems.size());
    loader::MinLogLevel = static_cast<loader::LogLevel>(g_config.log_level);
    if (result.config_state == CMFM_STRACKER_CONFIG_FOUND) loader::LOG(loader::INFO) << "Found config file";
    if (result.config_state == CMFM_STRACKER_CONFIG_INVALID) {
        loader::LOG(loader::ERR) << "loader-config.json is not valid JSON; using the defaults";
    }
    for (const auto& problem : problems) loader::LOG(loader::ERR) << "loader-config.json: " << problem;
}

void old_warning(const std::wstring& game) {
    if (file_exists(game + L"\\dtdata.dll") || file_exists(game + L"\\hid.dll")) {
        loader::LOG(loader::ERR) << "Found old dlls in game folder.";
        loader::LOG(loader::ERR) << "Please delete hid.dll and/or dtdata.dll from the game folder if they exist.";
        loader::LOG(loader::ERR) << "They are obsolete files from previous versions of this mod.";
    }
}

void version_failed(const char* detail) {
    loader::LOG(loader::ERR) << "Build Number check failed.";
    loader::LOG(loader::ERR) << "Could not find game version";
    loader::LOG(loader::ERR) << "Loader needs to be updated.";
    loader::LOG(loader::ERR) << "(" << detail << ")";
}

void find_version(const void* image, cmfm_stracker_result& result) {
    const auto* base = static_cast<const std::uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || !readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        !readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
        version_failed("the game image header is unreadable");
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;

    // Stracker scans every committed region of the image and needs exactly one match.
    const auto pattern = parse_bits(kVersionPattern);
    std::vector<std::size_t> found;
    for (std::size_t offset = 0; offset < size && found.size() < 2;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(base + offset, &info, sizeof(info))) break;
        const auto region_end = std::min<std::size_t>(
            size, static_cast<std::size_t>(static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize - base));
        if (region_end <= offset) break;
        if (readable(base + offset, region_end - offset)) {
            find_all({base + offset, region_end - offset}, pattern, offset, found, 2);
        }
        offset = region_end;
    }
    if (found.size() != 1) {
        version_failed(found.empty() ? "pattern not found" : "pattern found more than once");
        return;
    }

    const auto* instruction = base + found[0] - kVersionInstructionBack;
    if (found[0] < kVersionInstructionBack || !readable(instruction, 7)) {
        version_failed("the version instruction is unreadable");
        return;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 3, sizeof(displacement));
    const auto pointer_address =
        version_pointer_address(reinterpret_cast<std::uintptr_t>(base + found[0]), displacement);
    const auto* pointer = reinterpret_cast<const char* const*>(pointer_address);
    if (pointer_address < reinterpret_cast<std::uintptr_t>(base) ||
        pointer_address + sizeof(void*) > reinterpret_cast<std::uintptr_t>(base) + size ||
        !readable(pointer, sizeof(*pointer))) {
        version_failed("the version pointer is outside the game image");
        return;
    }
    const char* text = *pointer;
    if (!text) {
        version_failed("the version pointer is not set yet");
        return;
    }
    std::string version;
    for (std::size_t k = 0; k < 64 && readable(text + k, 1) && text[k]; ++k) version += text[k];
    if (!plausible_version(version)) {
        version_failed("the version text is not readable text");
        return;
    }
    g_version = version;
    loader::GameVersion = g_version.c_str();
    result.version_found = 1;
    copy_text(result.game_version, sizeof(result.game_version), g_version);
    loader::LOG(loader::INFO) << "Game version " << loader::GameVersion;
}

void load_plugins(const std::wstring& game, cmfm_stracker_result& result) {
    result.plugin_loader_enabled = g_config.enable_plugin_loader ? 1 : 0;
    if (!g_config.enable_plugin_loader) return;
    const auto folder = game + L"\\nativePC\\plugins";
    std::vector<DirectoryEntry> entries;
    WIN32_FIND_DATAW entry{};
    const HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
                                         nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;  // Stracker: no folder, no plugins
    do {
        const std::wstring name = entry.cFileName;
        if (name != L"." && name != L"..") {
            entries.push_back({name, (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    for (const auto& name : plugin_files(std::move(entries))) {
        ++result.plugins_found;
        // Stracker logs the relative path, quoted, as std::filesystem prints it.
        const auto shown = std::filesystem::path(L"nativePC\\plugins") / name;
        try {
            loader::LOG(loader::INFO) << "Loading plugin " << shown;
        } catch (...) { /* A name the ANSI code page cannot hold; load it anyway. */ }
        // A damaged DLL would make Windows show a "Bad Image" box and wait for
        // a click; the failure is reported through CMFM instead. Only this
        // thread's error mode changes, and only while it loads the plugin.
        DWORD previous_mode = 0;
        const bool quiet = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
        const HMODULE plugin = LoadLibraryW((folder + L"\\" + name).c_str());
        const DWORD error = GetLastError();
        if (quiet) SetThreadErrorMode(previous_mode, nullptr);
        if (plugin) {
            ++result.plugins_loaded;
            continue;
        }
        if (!result.plugins_failed++) copy_text(result.first_failure, sizeof(result.first_failure), utf8(name));
        try {
            loader::LOG(loader::ERR) << "Failed to load " << shown;
        } catch (...) {
            loader::LOG(loader::ERR) << "Failed to load nativePC\\plugins\\" << utf8(name);
        }
        loader::LOG(loader::ERR) << "(Windows error " << error << ")";
    }
}

void start(const std::wstring& game, const void* image, cmfm_stracker_result& result) {
    {
        Lock lock;
        g_log_path = game + L"\\loader.log";
    }
    load_config(game, result);
    old_warning(game);
    find_version(image, result);
    load_plugins(game, result);
}

std::wstring exe_folder() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}
}  // namespace

loader::LOG::~LOG() {
    try {
        stream << std::endl;
        stream.flush();
        write_line(static_cast<int>(logLevel), stream.str());
    } catch (...) { /* A destructor must not throw. */ }
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl cmfm_stracker_start(const cmfm_stracker_host* host,
                                                                           cmfm_stracker_result* result) noexcept {
    if (!host || host->size < sizeof(cmfm_stracker_host) || host->version != CMFM_STRACKER_HOST_VERSION ||
        !host->game_folder || !result || result->size < sizeof(cmfm_stracker_result)) {
        return 2;
    }
    const auto size = result->size;
    *result = {};
    result->size = size;
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        result->already_started = 1;
        return 1;
    }
    try {
        {
            Lock lock;
            g_sink = host->log;
            g_sink_context = host->log_context;
        }
        start(host->game_folder, host->game_image ? host->game_image : GetModuleHandleW(nullptr), *result);
    } catch (...) {
        write_line(loader::ERR, "Stracker start-up stopped by an unexpected error\n");
    }
    return 0;
}

// Stracker's own entry point, kept so this DLL can stand in for loader.dll
// anywhere. After cmfm_stracker_start, or a first call, it does nothing.
extern "C" __declspec(dllexport) void Initialize() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    try {
        cmfm_stracker_result result{};
        result.size = sizeof(result);
        const auto game = exe_folder();
        if (!game.empty()) start(game, GetModuleHandleW(nullptr), result);
    } catch (...) { /* Stracker showed a message box here; the game continues. */ }
}
