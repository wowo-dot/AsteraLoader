#include "common/launch_plan.hpp"
#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"

#include <windows.h>
#include <shellapi.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto output = cmfm::environment(L"CMFM_FAKE_OUTPUT");
    if (output.empty()) return 90;
    cmfm::LogFile log;
    if (!log.open(output)) return 91;
    const auto core = GetModuleHandleW(L"cmfm.dll");
    log.line(core ? L"core_loaded_before_main=1" : L"core_loaded_before_main=0");
    std::wstring cwd(32768, L'\0');
    const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data());
    if (!length || length >= cwd.size()) return 92;
    cwd.resize(length);
    log.line(L"cwd=" + cwd);
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 93;
    for (int i = 1; i < argc; ++i) {
        log.line(L"arg=" + cmfm::quote_argument(argv[i]));
    }
    LocalFree(argv);
    const auto delay = cmfm::environment(L"CMFM_FAKE_SLEEP_MS");
    if (!delay.empty()) Sleep(static_cast<DWORD>(std::stoul(delay)));
    const auto code = cmfm::environment(L"CMFM_FAKE_EXIT");
    return code.empty() ? 37 : std::stoi(code);
}
