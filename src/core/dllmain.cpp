#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/sha256.hpp"
#include "common/startup.hpp"
#include "common/windows.hpp"

#include <windows.h>

#include <string>

namespace {
HMODULE g_module = nullptr;
LARGE_INTEGER g_attach_qpc{};
FILETIME g_attach_time{};
volatile LONG g_started = 0;

DWORD startup(const cmfm::StartupRequest* request) {
    if (!request || request->size != sizeof(*request) || request->abi != cmfm::kStartupAbi) {
        return ERROR_REVISION_MISMATCH;
    }
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return ERROR_ALREADY_INITIALIZED;

    // This export is called after LoadLibraryW has returned, outside DllMain
    // and the loader lock. No plugin code or game hooks run in this skeleton.
    const auto virtual_path = cmfm::module_path(g_module);
    cmfm::Handle file(CreateFileW(virtual_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return GetLastError();
    const auto real_path = cmfm::final_path(file.get());
    if (real_path.empty()) return GetLastError();
    const auto folder = cmfm::join_path(cmfm::directory_of(real_path), L"logs");
    cmfm::LogFile log;
    const auto log_path = cmfm::join_path(folder, L"core." +
                                        std::to_wstring(GetCurrentProcessId()) + L".log");
    if (!log.open(log_path)) return GetLastError();

    const auto profile = cmfm::environment(L"USERPROFILE");
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    log.line(L"CMFM " CMFM_VERSION L" commit=" CMFM_GIT_COMMIT);
    log.line(L"pid=" + std::to_wstring(GetCurrentProcessId()));
    log.line(L"dll=" + cmfm::redact_profile(real_path, profile));
    const auto dll_hash = cmfm::sha256_file(real_path);
    log.line(L"dll_sha256=" + dll_hash);
    const auto exe = cmfm::module_path();
    log.line(L"exe=" + cmfm::redact_profile(exe, profile));
    const auto exe_hash = cmfm::sha256_file(exe);
    log.line(L"exe_sha256=" + exe_hash);
    if (exe_hash.empty() || dll_hash.empty()) {
        log.line(L"startup_failed: file hash unavailable");
        return ERROR_READ_FAULT;
    }
    log.line(L"attach_qpc=" + std::to_wstring(g_attach_qpc.QuadPart));
    log.line(L"bootstrap_qpc=" + std::to_wstring(now.QuadPart));
    log.line(L"qpc_frequency=" + std::to_wstring(frequency.QuadPart));
    log.line(L"launcher_qpc=" + std::to_wstring(request->launcher_qpc));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
                              created.dwLowDateTime;
        log.line(L"process_creation_filetime=" + std::to_wstring(creation));
        const auto attach = (static_cast<std::uint64_t>(g_attach_time.dwHighDateTime) << 32) |
                             g_attach_time.dwLowDateTime;
        if (attach >= creation) log.line(L"attach_us_after_process_creation=" + std::to_wstring((attach - creation) / 10));
    }
    for (const auto name : {L"usvfs_x64.dll", L"winmm.dll", L"steamclient64.dll",
                            L"steam_api64.dll", L"gameoverlayrenderer64.dll", L"dxgi.dll",
                            L"d3d12.dll", L"dinput8.dll", L"loader.dll"}) {
        const auto module = GetModuleHandleW(name);
        log.line(std::wstring(L"module ") + name + L"=" +
                 (module ? cmfm::redact_profile(cmfm::module_path(module), profile) : L"absent"));
    }
    log.line(L"MO2 profile and force-load: record in the run manifest; not inferred here");
    log.line(L"CMFM plugins loaded: none (startup proof only)");
    log.line(L"startup_confirmed; no game hooks installed");
    return log.good() ? cmfm::kStartupOk : ERROR_WRITE_FAULT;
}
}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI cmfm_bootstrap(void* parameter) noexcept {
    try {
        return startup(static_cast<const cmfm::StartupRequest*>(parameter));
    } catch (...) {
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        QueryPerformanceCounter(&g_attach_qpc);
        GetSystemTimePreciseAsFileTime(&g_attach_time);
        // Static CRT needs thread notifications; do not disable them here.
    }
    return TRUE;
}
