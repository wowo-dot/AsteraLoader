// Runs the plugin host against fixture plugins laid out in a temporary
// plugins folder: argv[1] holds the built fixtures, argv[2] is a work folder.
#include "work_dir.hpp"
#include "common/log_file.hpp"
#include "common/sha256.hpp"
#include "common/windows.hpp"
#include "core/plugins/host.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

const al::plugins::PluginReport* find(const std::vector<al::plugins::PluginReport>& report,
                                         const std::wstring& name) {
    for (const auto& line : report) {
        if (line.name == name) return &line;
    }
    return nullptr;
}

void expect_refused(const std::vector<al::plugins::PluginReport>& report, const std::wstring& name,
                    const std::string& reason_part) {
    const auto label = al::to_utf8(name) + " refused";
    const auto line = find(report, name);
    check(line && line->status == AL_STATUS_DISABLED && !line->initialised &&
          line->reason.find(reason_part) != std::string::npos, label);
    if (line && line->reason.find(reason_part) == std::string::npos) std::cerr << "  reason: " << line->reason << '\n';
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const fs::path fixtures = argv[1];
    const fs::path work = al_test::fresh_work_dir(argv[2]);
    const auto root = work / L"plugins";
    const auto logs = work / L"logs";
    fs::create_directories(root);

    const auto install = [&](const wchar_t* fixture, const wchar_t* folder) {
        fs::create_directories(root / folder);
        fs::copy_file(fixtures / (std::wstring(fixture) + L".dll"), root / folder / (std::wstring(folder) + L".dll"));
    };
    for (const auto name : {L"alpha", L"beta", L"gamma", L"delta", L"cycle_x", L"cycle_y", L"newer_api",
                            L"failing", L"wrong_exe", L"no_exports", L"phased"}) {
        install(name, name);
    }
    install(L"alpha", L"mismatch");  // declares the name "alpha" in folder "mismatch"
    fs::create_directories(root / L"Bad Folder");
    fs::create_directories(root / L"empty");
    fs::create_directories(root / L"damaged");
    std::ofstream(root / L"damaged" / L"damaged.dll") << "not a DLL";  // must not raise a Windows dialog
    fs::copy_file(fixtures / L"alpha.dll", root / L"stray.dll");

    const auto record = work / L"record.txt";
    SetEnvironmentVariableW(L"AL_TEST_PLUGIN_RECORD", record.c_str());

    al::plugins::Host empty_host;
    empty_host.load_all({(work / L"missing").wstring(), logs.wstring(), L""});
    check(empty_host.report().empty(), "missing plugins folder means no plugins");

    al::plugins::Host host;
    host.load_all({root.wstring(), logs.wstring(), al::sha256_file(al::module_path())});
    const auto report = host.report();

    std::vector<std::wstring> initialised;
    for (const auto& line : report) {
        if (line.initialised) initialised.push_back(line.name);
    }
    check(initialised == std::vector<std::wstring>{L"alpha", L"failing", L"phased", L"gamma", L"delta", L"beta"},
          "init order: order numbers, then names, with load_after / load_before applied");
    check(read_file(record) == "alpha init\nfailing init\nphased init\ngamma init\ndelta init\nbeta init\n",
          "plugins saw init in that order with both folder paths");

    for (const auto name : {L"alpha", L"phased", L"gamma", L"delta", L"beta"}) {
        const auto line = find(report, name);
        check(line && line->status == AL_STATUS_ACTIVE && line->reason.empty() && line->version == "0.0.1" &&
              line->display_name == "Fixture " + al::to_utf8(name), al::to_utf8(name) + " active");
    }
    const auto failing = find(report, L"failing");
    check(failing && failing->initialised && failing->status == AL_STATUS_DISABLED &&
          failing->reason == "fixture refuses to start", "failing init keeps the plugin's own reason");

    expect_refused(report, L"cycle_x", "cycle with cycle_y");
    expect_refused(report, L"cycle_y", "cycle with cycle_x");
    expect_refused(report, L"newer_api", "Update Astera Loader");
    expect_refused(report, L"wrong_exe", "does not support this game exe");
    expect_refused(report, L"no_exports", "not an Astera Loader plugin");
    expect_refused(report, L"mismatch", "does not match its folder 'mismatch'");
    expect_refused(report, L"Bad Folder", "not a valid plugin name");
    expect_refused(report, L"empty", "no empty.dll");
    expect_refused(report, L"damaged", "Windows could not load it");
    expect_refused(report, L"stray.dll", "own folder");
    check(report.size() == 16, "every entry reported");

    // Refused plugins were unloaded again; initialised ones stay loaded.
    for (const auto name : {L"cycle_x", L"newer_api", L"wrong_exe", L"no_exports", L"mismatch"}) {
        const auto dll = (root / name / (std::wstring(name) + L".dll")).wstring();
        check(GetModuleHandleW(dll.c_str()) == nullptr, al::to_utf8(name) + " unloaded");
    }
    for (const auto name : {L"alpha", L"failing", L"beta"}) {
        const auto dll = (root / name / (std::wstring(name) + L".dll")).wstring();
        check(GetModuleHandleW(dll.c_str()) != nullptr, al::to_utf8(name) + " stays loaded");
    }

    // Phases reach running plugins that asked for them, never disabled ones.
    fs::remove(record);
    host.dispatch(AL_PHASE_PRE_MAIN, nullptr);
    host.dispatch(AL_PHASE_WIN_MAIN, nullptr);
    host.dispatch(AL_PHASE_GAME_READY, nullptr);
    host.dispatch(AL_PHASE_COUNT, nullptr);
    check(read_file(record) == "phased phase 0\nphased phase 5\n", "phase dispatch");

    const auto alpha_log = read_file(logs / L"plugins" / L"alpha.log");
    check(alpha_log.find(" INFO fixture alpha started\r\n") != std::string::npos, "plugin log line");
    const auto failing_log = read_file(logs / L"plugins" / L"failing.log");
    check(failing_log.find(" STATUS disabled: fixture refuses to start\r\n") != std::string::npos,
          "status change logged");

    if (failures) {
        std::cerr << failures << " host check(s) failed\n";
        return 1;
    }
    std::cout << "Plugin host checks passed\n";
    return 0;
}
