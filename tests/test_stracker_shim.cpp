// Runs CMFM's loader.dll shim against a fake game folder:
// argv[1] the shim, argv[2] the stand-in Stracker plugin, argv[3] a work folder.
#include "shim.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// Stracker's game-version pattern planted in this exe's data, with the
// instruction 0x18 bytes before it pointing at g_planted_version.
std::uint8_t g_planted[0x18 + 9] = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x42, 0xF5};
const char* g_planted_version = "test-421";

std::vector<std::pair<int, std::string>> g_lines;
void __cdecl collect(void* context, int level, const char* line) {
    if (context == &g_lines) g_lines.emplace_back(level, line);
}

bool logged(int level, const std::string& text) {
    for (const auto& [line_level, line] : g_lines) {
        if (line_level == level && line == text) return true;
    }
    std::cerr << "  not sent to CMFM at level " << level << ": " << text << '\n';
    return false;
}

constexpr const char* kExports[] = {"??0LOG@loader@@QEAA@W4LogLevel@1@@Z", "??1LOG@loader@@QEAA@XZ",
                                    "?MinLogLevel@loader@@3W4LogLevel@1@A", "?GameVersion@loader@@3PEBDEB",
                                    "Initialize", CMFM_STRACKER_START_EXPORT};
}  // namespace

int wmain(int argc, wchar_t** argv) {
    // argv[4] "known": CMFM passes the game version (host version 2) instead of the shim searching.
    if (argc != 4 && argc != 5) return 2;
    const bool known = argc == 5 && std::wstring(argv[4]) == L"known";
    const std::string version_text = known ? "known-7" : "test-421";
    const fs::path shim_path = argv[1], fixture = argv[2];
    const auto work = fs::path(argv[3]) / std::to_wstring(GetCurrentProcessId());
    const auto game = work / L"game";
    const auto plugins = game / L"nativePC" / L"plugins";
    fs::create_directories(plugins / L"folder.dll");
    std::ofstream(game / L"loader-config.json")
        << R"({"logfile": true, "logcmd": false, "logLevel": "DEBUG", "SPL": {"PreloadDLLs": ["dinput8.dll"]}})";
    std::ofstream(game / L"hid.dll") << "old";
    for (const auto name : {L"Zeta.dll", L"!early.dll", L"alpha.dll", L"skip.DLL"}) fs::copy_file(fixture, plugins / name);
    std::ofstream(plugins / L"broken.dll") << "not a DLL";
    std::ofstream(plugins / L"readme.txt") << "text";
    const auto record = work / L"record.txt";
    SetEnvironmentVariableW(L"CMFM_TEST_STRACKER_RECORD", record.c_str());
    // The shim loads nativePC\plugins\<name> by Stracker's relative path. In the
    // game Windows finds it through the exe's folder; this test exe lives
    // elsewhere, so the current folder stands in for it.
    SetCurrentDirectoryW(game.c_str());

    const auto displacement = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(&g_planted_version) -
                                                        reinterpret_cast<std::intptr_t>(g_planted + 7));
    std::memcpy(g_planted + 3, &displacement, sizeof(displacement));

    const HMODULE shim = LoadLibraryW(shim_path.c_str());
    if (!shim) return 3;
    for (const auto name : kExports) check(GetProcAddress(shim, name) != nullptr, std::string("export ") + name);
    const auto start = reinterpret_cast<cmfm_stracker_start_fn>(GetProcAddress(shim, CMFM_STRACKER_START_EXPORT));
    if (!start) return 4;

    const auto game_text = game.wstring();
    cmfm_stracker_host host{sizeof(host), CMFM_STRACKER_HOST_VERSION, collect, &g_lines, game_text.c_str(),
                            GetModuleHandleW(nullptr), known ? "known-7" : nullptr};
    cmfm_stracker_result result{};
    result.size = sizeof(result);
    check(start(&host, &result) == 0, "start");
    check(result.config_state == CMFM_STRACKER_CONFIG_FOUND && result.config_problems == 0 &&
          result.plugin_loader_enabled, "config read, SPL object ignored");
    check(result.plugins_found == 4 && result.plugins_loaded == 3 && result.plugins_failed == 1 &&
          std::string(result.first_failure) == "broken.dll", "four *.dll files found, three loaded, broken.dll failed");
    check(result.version_found && std::string(result.game_version) == version_text, "game version found");

    // Stracker's order, and only exact ".dll" files.
    check(read_file(record) == "!early.dll\nalpha.dll\nZeta.dll\n", "load order !early, alpha, Zeta");
    check(!GetModuleHandleW((plugins / L"skip.DLL").c_str()), "skip.DLL not loaded, as Stracker would not");

    // Every plugin bound its loader.dll import to this shim, though no
    // loader.dll is beside it.
    const auto shim_min_level = GetProcAddress(shim, "?MinLogLevel@loader@@3W4LogLevel@1@A");
    check(GetModuleHandleW(L"loader.dll") == shim, "loader.dll is the shim");
    check(*reinterpret_cast<const int*>(shim_min_level) == 0, "MinLogLevel from logLevel DEBUG");
    for (const auto name : {L"!early.dll", L"alpha.dll", L"Zeta.dll"}) {
        const auto plugin = GetModuleHandleW((plugins / name).c_str());
        using MinLevel = const void* (*)();
        using Version = const char* (*)();
        const auto min_level = reinterpret_cast<MinLevel>(GetProcAddress(plugin, "fixture_min_level"));
        const auto version = reinterpret_cast<Version>(GetProcAddress(plugin, "fixture_game_version"));
        check(plugin && min_level && min_level() == reinterpret_cast<const void*>(shim_min_level) && version &&
              std::string(version()) == version_text, "plugin import bound to the shim");
    }

    // Stracker's log texts, in loader.log and in CMFM's log.
    const auto log = read_file(game / L"loader.log");
    for (const auto& text : std::vector<std::string>{"Found config file", "Found old dlls in game folder.",
                                                    "Game version " + version_text,
                            R"(Loading plugin "nativePC\\plugins\\!early.dll")",
                            "fixture !early.dll sees game version " + version_text, "fixture debug line 42",
                            R"(Failed to load "nativePC\\plugins\\broken.dll")"}) {
        const auto line = std::string(" ] ") + text + "\r\n";
        check(log.find(line) != std::string::npos, std::string("loader.log has: ") + text);
    }
    // Load times go to CMFM only, so loader.log stays as Stracker writes it.
    bool timed = false;
    for (const auto& [level, line] : g_lines) timed = timed || (level == 1 && line.rfind("loaded alpha.dll in ", 0) == 0);
    check(timed && log.find("loaded alpha.dll in ") == std::string::npos, "load time for CMFM only");
    check(log.rfind("[ ", 0) == 0 && log.find("\n[ ") != std::string::npos, "loader.log lines start with \"[ \"");
    check(logged(1, "Found config file") && logged(3, "Found old dlls in game folder.") &&
          logged(3, "fixture alpha.dll sees game version " + version_text) && logged(0, "fixture debug line 42") &&
          logged(3, R"(Failed to load "nativePC\\plugins\\broken.dll")"), "lines sent to CMFM with levels");

    // A leftover Stracker loader asks for the same relative path later: it
    // must get the module already loaded, not a second copy.
    const auto again_alpha = LoadLibraryW(L"nativePC\\plugins\\alpha.dll");
    check(again_alpha && again_alpha == GetModuleHandleW((plugins / L"alpha.dll").c_str()) &&
          read_file(record) == "!early.dll\nalpha.dll\nZeta.dll\n", "Stracker's relative load finds the plugin loaded");
    if (again_alpha) FreeLibrary(again_alpha);

    // It runs once; Stracker's Initialize does nothing afterwards.
    cmfm_stracker_result again{};
    again.size = sizeof(again);
    check(start(&host, &again) == 1 && again.already_started, "second start refused");
    const auto initialize = reinterpret_cast<void (*)()>(GetProcAddress(shim, "Initialize"));
    if (initialize) initialize();
    check(read_file(record) == "!early.dll\nalpha.dll\nZeta.dll\n", "Initialize after start loads nothing");

    if (failures) {
        std::cerr << failures << " Stracker shim check(s) failed\n";
        return 1;
    }
    std::cout << "Stracker shim: exports, config, version, order, binding and logs checked\n";
    return 0;
}
