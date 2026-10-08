#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/sha256.hpp"
#include "common/startup.hpp"
#include "common/windows.hpp"
#include "core/startup/runtime.hpp"

#include <windows.h>

#include <string>
#include <string_view>

namespace {
HMODULE g_module = nullptr;
LARGE_INTEGER g_attach_qpc{};
FILETIME g_attach_time{};
volatile LONG g_started = 0;

// The core log stays open for the whole process: the exe checksum thread
// writes to it after the bootstrap has returned.
cmfm::LogFile g_log;
SRWLOCK g_log_lock = SRWLOCK_INIT;

class LogLock {
public:
    LogLock() { AcquireSRWLockExclusive(&g_log_lock); }
    ~LogLock() { ReleaseSRWLockExclusive(&g_log_lock); }
    LogLock(const LogLock&) = delete;
    LogLock& operator=(const LogLock&) = delete;
};

void log_line(std::wstring_view text) {
    LogLock lock;
    g_log.line(text);
}

// Hashing the 84 MB exe took 229 ms of the game's pause in run
// launcher-mo2-1. The launcher logs the same hash, so here it is diagnostic:
// it runs at background priority while the game starts, and a failure is
// only a warning.
DWORD WINAPI log_exe_hash(void*) noexcept {
    try {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        const auto hash = cmfm::sha256_file(cmfm::module_path());
        LARGE_INTEGER done{};
        QueryPerformanceCounter(&done);
        try {
            log_line(hash.empty() ? L"warning: exe SHA-256 unavailable" : L"exe_sha256=" + hash);
            log_line(L"exe_sha256_qpc=" + std::to_wstring(done.QuadPart));
        } catch (...) { /* The plugin hash result is independent of log writes. */ }
        cmfm::startup::publish_hash_result(hash);
    } catch (...) {
        cmfm::startup::publish_hash_result({});
        try {
            log_line(L"warning: exe SHA-256 logging failed");
        } catch (...) { /* Nothing left to report to. */ }
    }
    FreeLibraryAndExitThread(g_module, 0);
}

void start_exe_hash() {
    // The thread holds its own reference to this DLL and drops it with
    // FreeLibraryAndExitThread, so the DLL cannot unmap under it.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&log_exe_hash), &self)) {
        log_line(L"warning: exe SHA-256 not started: " + cmfm::win32_error_text(GetLastError()));
        cmfm::startup::publish_hash_result({});
        return;
    }
    cmfm::Handle thread(CreateThread(nullptr, 0, log_exe_hash, nullptr, 0, nullptr));
    if (!thread) {
        const DWORD code = GetLastError();
        FreeLibrary(self);
        log_line(L"warning: exe SHA-256 not started: " + cmfm::win32_error_text(code));
        cmfm::startup::publish_hash_result({});
    }
}

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
    const auto log_path = cmfm::join_path(folder, L"core." +
                                        std::to_wstring(GetCurrentProcessId()) + L".log");
    {
        LogLock lock;
        if (!g_log.open(log_path)) return GetLastError();
    }

    const auto profile = cmfm::environment(L"USERPROFILE");
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    log_line(L"CMFM " CMFM_VERSION L" commit=" CMFM_GIT_COMMIT);
    log_line(L"pid=" + std::to_wstring(GetCurrentProcessId()));
    log_line(L"dll=" + cmfm::redact_profile(real_path, profile));
    const auto dll_hash = cmfm::sha256_file(real_path);
    log_line(L"dll_sha256=" + dll_hash);
    log_line(L"exe=" + cmfm::redact_profile(cmfm::module_path(), profile));
    if (dll_hash.empty()) {
        log_line(L"startup_failed: DLL hash unavailable");
        return ERROR_READ_FAULT;
    }
    log_line(L"attach_qpc=" + std::to_wstring(g_attach_qpc.QuadPart));
    log_line(L"bootstrap_qpc=" + std::to_wstring(now.QuadPart));
    log_line(L"qpc_frequency=" + std::to_wstring(frequency.QuadPart));
    log_line(L"launcher_qpc=" + std::to_wstring(request->launcher_qpc));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
                              created.dwLowDateTime;
        log_line(L"process_creation_filetime=" + std::to_wstring(creation));
        const auto attach = (static_cast<std::uint64_t>(g_attach_time.dwHighDateTime) << 32) |
                             g_attach_time.dwLowDateTime;
        if (attach >= creation) log_line(L"attach_us_after_process_creation=" + std::to_wstring((attach - creation) / 10));
    }
    for (const auto name : {L"usvfs_x64.dll", L"winmm.dll", L"steamclient64.dll",
                            L"steam_api64.dll", L"gameoverlayrenderer64.dll", L"dxgi.dll",
                            L"d3d12.dll", L"dinput8.dll", L"loader.dll"}) {
        const auto module = GetModuleHandleW(name);
        log_line(std::wstring(L"module ") + name + L"=" +
                 (module ? cmfm::redact_profile(cmfm::module_path(module), profile) : L"absent"));
    }
    log_line(L"MO2 profile and force-load: record in the run manifest; not inferred here");
    log_line(L"CMFM plugins deferred until verified PRE_MAIN");
    log_line(L"startup_confirmed; game startup phase not yet entered");
    {
        LogLock lock;
        if (!g_log.good()) return ERROR_WRITE_FAULT;
    }
    cmfm::startup::initialise_hash_result();
    start_exe_hash();
    cmfm::startup::arm_pre_main(g_module, folder, log_line);
    return cmfm::kStartupOk;
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
