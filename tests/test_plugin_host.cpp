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
    install(L"alpha", L"al");        // AL's service namespace
    install(L"alpha", L"al.tools");
    install(L"alpha", L"dinput8");   // a proxy DLL name
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
    expect_refused(report, L"al", "reserved for Astera Loader's own services");
    expect_refused(report, L"al.tools", "reserved for Astera Loader's own services");
    expect_refused(report, L"dinput8", "reserved for the game, Windows or another loader");
    check(report.size() == 19, "every entry reported");

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
    // EXITING goes out once, whichever path raises it first.
    check(host.raise_exiting() && !host.raise_exiting(), "EXITING raised once");

    const auto alpha_log = read_file(logs / L"plugins" / L"alpha.log");
    check(alpha_log.find(" INFO fixture alpha started\r\n") != std::string::npos, "plugin log line");
    const auto failing_log = read_file(logs / L"plugins" / L"failing.log");
    check(failing_log.find(" STATUS disabled: fixture refuses to start\r\n") != std::string::npos,
          "status change logged");

    // The start-up popup: which plugins are not fully active, and why.
    using al::plugins::attention_notice;
    using al::plugins::PluginReport;
    const auto npos = std::wstring::npos;
    check(attention_notice({}).empty(), "no notice without plugins");
    check(attention_notice(host.report()).find(L"(not running): fixture refuses to start") != npos,
          "the host's disabled fixture is named with its reason");
    PluginReport degraded;
    degraded.name = L"stracker_compat";
    degraded.display_name = "Stracker's Loader compatibility";
    degraded.status = AL_STATUS_DEGRADED;
    degraded.reason = "Stracker's Loader files are still in the game folder";
    auto active = degraded;
    active.status = AL_STATUS_ACTIVE;
    check(attention_notice({active}).empty(), "no notice when every plugin is active");
    check(attention_notice({active, degraded}) ==
              L"Astera Loader: 1 plugin needs attention.\n\nStracker's Loader compatibility (some features off): "
              L"Stracker's Loader files are still in the game folder\n",
          "one degraded plugin, by display name, with its reason");
    std::vector<PluginReport> many(7, degraded);
    many[0].reason = std::string(300, 'x');
    many[1].reason.clear();
    many[1].display_name.clear();
    const auto crowded = attention_notice(many);
    std::size_t shown = 0;
    for (auto at = crowded.find(L"(some features off)"); at != npos; at = crowded.find(L"(some features off)", at + 1)) ++shown;
    check(crowded.find(L"7 plugins need attention") != npos && shown == 5 && crowded.find(L"\nand 2 more.") != npos,
          "seven plugins: five listed, then how many more");
    check(crowded.find(std::wstring(200, L'x') + L"\x2026") != npos && crowded.find(std::wstring(201, L'x')) == npos,
          "a long reason is cut at 200 characters");
    check(crowded.find(L"\nstracker_compat (some features off): no reason given") != npos,
          "no display name: the folder name; no reason: said so");

    if (failures) {
        std::cerr << failures << " host check(s) failed\n";
        return 1;
    }
    std::cout << "Plugin host checks passed\n";
    return 0;
}
