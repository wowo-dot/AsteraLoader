#include "common/windows.hpp"
#include "core/plugins/host.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const fs::path fixtures = argv[1];
    const auto work = fs::path(argv[2]) / std::to_wstring(GetCurrentProcessId());
    const auto root = work / "plugins";
    for (const auto name : {L"seh_info", L"seh_init", L"seh_phase", L"cpp_init", L"phased"}) {
        fs::create_directories(root / name);
        fs::copy_file(fixtures / (std::wstring(name) + L".dll"), root / name / (std::wstring(name) + L".dll"));
    }
    fs::create_directories(root / "loader");
    fs::copy_file(fixtures / L"alpha.dll", root / L"loader/loader.dll");
    const auto record = (work / "record.txt").wstring();
    SetEnvironmentVariableW(L"CMFM_TEST_PLUGIN_RECORD", record.c_str());
    cmfm::plugins::Host host;
    host.load_all({root.wstring(), (work / "logs").wstring(), L""});
    int failures = 0;
    const auto check_status = [&](const wchar_t* name, bool initialised, const char* reason) {
        const auto report = host.report();
        for (const auto& line : report) {
            if (line.name == name && line.initialised == initialised && line.status == CMFM_STATUS_DISABLED &&
                line.reason.find(reason) != std::string::npos) return;
        }
        ++failures; std::wcerr << L"FAIL: " << name << L" fault status\n";
    };
    check_status(L"seh_info", false, "metadata raised exception");
    check_status(L"seh_init", true, "cmfm_plugin_init raised exception");
    check_status(L"cpp_init", true, "cmfm_plugin_init raised exception");
    check_status(L"loader", false, "reserved");
    host.dispatch(CMFM_PHASE_PRE_MAIN, nullptr);
    check_status(L"seh_phase", true, "on_phase raised exception");
    host.dispatch(CMFM_PHASE_PRE_MAIN, nullptr);
    std::ifstream file(record);
    std::ostringstream text; text << file.rdbuf();
    if (text.str() != "phased init\nseh_phase init\nphased phase 0\nphased phase 0\n") {
        ++failures; std::cerr << "FAIL: healthy plugin stopped after another plugin fault\n" << text.str();
    }
    if (!failures) std::cout << "Native plugin fault and module-name checks passed\n";
    return failures ? 1 : 0;
}
