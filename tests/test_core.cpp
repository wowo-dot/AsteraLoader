#include "common/startup.hpp"
#include "common/windows.hpp"

#include <windows.h>

#include <bit>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const auto module = LoadLibraryW(argv[1]);
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
    FreeLibrary(module);
    if (passed) std::cout << "Core ABI and duplicate-start checks passed\n";
    return passed ? 0 : 4;
}
