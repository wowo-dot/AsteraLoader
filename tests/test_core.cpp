#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/sha256.hpp"
#include "common/startup.hpp"
#include "common/windows.hpp"

#include <windows.h>

#include <bit>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
// Polls for up to `ms`, so the tests do not depend on thread timing.
template <typename Check>
bool eventually(Check check, DWORD ms = 10000) {
    for (DWORD waited = 0; waited < ms; waited += 20) {
        if (check()) return true;
        Sleep(20);
    }
    return check();
}

std::string read_file(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const std::wstring dll = argv[1];
    const auto module = LoadLibraryW(dll.c_str());
    if (!module) return 2;
    using Bootstrap = DWORD(WINAPI*)(void*);
    const auto function = GetProcAddress(module, cmfm::kBootstrapExport);
    if (!function) { FreeLibrary(module); return 3; }
    const auto bootstrap = std::bit_cast<Bootstrap>(function);
    cmfm::StartupRequest request;
    bool passed = bootstrap(nullptr) == ERROR_REVISION_MISMATCH;
    request.size = 0;
    passed = passed && bootstrap(&request) == ERROR_REVISION_MISMATCH;
    request.size = sizeof(request);
    request.abi = 999;
    passed = passed && bootstrap(&request) == ERROR_REVISION_MISMATCH;
    request.abi = cmfm::kStartupAbi;
    passed = passed && bootstrap(&request) == cmfm::kStartupOk;
    passed = passed && bootstrap(&request) == ERROR_ALREADY_INITIALIZED;
    // The exe hash thread holds its own reference, so this does not unmap
    // the DLL under it.
    FreeLibrary(module);
    if (!passed) return 4;
    std::cout << "Core ABI and duplicate-start checks passed\n";

    // The hash is logged after the bootstrap returned, from the core's thread.
    const auto log_path = cmfm::join_path(cmfm::join_path(cmfm::directory_of(dll), L"logs"),
                                          L"core." + std::to_wstring(GetCurrentProcessId()) + L".log");
    const auto expected = "exe_sha256=" + cmfm::to_utf8(cmfm::sha256_file(cmfm::module_path()));
    if (!eventually([&] { return read_file(log_path).find(expected) != std::string::npos; })) {
        std::cout << "Core did not log this exe's SHA-256:\n" << read_file(log_path);
        return 5;
    }
    // When the thread ends, it drops its reference and the DLL unloads.
    if (!eventually([&] { return GetModuleHandleW(dll.c_str()) == nullptr; })) {
        std::cout << "Core DLL still loaded after the hash thread finished\n";
        return 6;
    }
    std::cout << "Core logged the exe hash after startup and released its DLL reference\n";
    return 0;
}
