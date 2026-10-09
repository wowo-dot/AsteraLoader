// Runs the hello example plugin through the plugin host:
// argv[1] is the built hello.dll, argv[2] a work folder.
#include "core/plugins/host.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const auto work = fs::path(argv[2]) / std::to_wstring(GetCurrentProcessId());
    fs::create_directories(work / L"plugins" / L"hello");
    fs::copy_file(argv[1], work / L"plugins" / L"hello" / L"hello.dll");

    cmfm::plugins::Host host;
    host.load_all({(work / L"plugins").wstring(), (work / L"logs").wstring(), L""});
    const auto report = host.report();
    const bool loaded = report.size() == 1 && report[0].name == L"hello" && report[0].initialised &&
                        report[0].status == CMFM_STATUS_ACTIVE && report[0].version == "0.3.0" &&
                        report[0].display_name == "CMFM Hello";
    host.dispatch(CMFM_PHASE_PRE_MAIN, nullptr);

    // Stand-in payloads; hello only logs them. The IID is ID3D12Device's.
    const cmfm_phase_win_main_t win_main{sizeof(win_main), 0, GetModuleHandleW(nullptr), "-test", SW_SHOWDEFAULT, 0};
    host.dispatch(CMFM_PHASE_WIN_MAIN, &win_main);
    static const GUID device_iid{0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
    int stand_in_device = 0;
    const cmfm_phase_device_t device{sizeof(device), 0, &stand_in_device, &device_iid, nullptr, 0xb000u, 0};
    host.dispatch(CMFM_PHASE_DEVICE_CREATED, &device);
    const cmfm_phase_shutdown_t shutdown{sizeof(shutdown), 7};
    host.dispatch(CMFM_PHASE_SHUTDOWN, &shutdown);

    std::ifstream file(work / L"logs" / L"plugins" / L"hello.log", std::ios::binary);
    std::ostringstream log;
    log << file.rdbuf();
    const auto text = log.str();
    bool logged = true;
    for (const auto part : {" INFO hello 0.3.0 initialised on thread ", "(CMFM API 1.2)",
                            " INFO folder as the game sees it: ", " INFO folder resolved through a file handle: ",
                            " INFO this DLL: ", " INFO data folder: ",
                            " INFO game version unknown, exe SHA-256 unknown..., graphics ",
                            " INFO version code found 0 time(s)", " INFO phase 0 on thread ",
                            " INFO game exe as the game sees it: ", " INFO phase 1 on thread ",
                            " INFO WinMain: instance is the game exe: yes, command line \"-test\", show command 10",
                            " INFO phase 2 on thread ",
                            " INFO device present, adapter default, minimum feature level 0xb000",
                            " INFO device interface the game asked for: {189819F1-1DB6-4B57-BE54-1821339B85F7}",
                            " INFO phase 6 on thread ", " INFO WinMain returned 7"}) {
        if (text.find(part) == std::string::npos) {
            std::cerr << "missing in hello.log: " << part << '\n';
            logged = false;
        }
    }
    if (!loaded) std::cerr << "hello was not loaded and initialised as expected\n";
    if (!loaded || !logged) {
        std::cerr << text;
        return 1;
    }
    std::cout << "hello example plugin loaded, initialised and logged every phase payload\n";
    return 0;
}
