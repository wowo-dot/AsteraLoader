// A test plugin, built once per FIXTURE_NAME with different declarations.
// It records init and phase calls in the file named by AL_TEST_PLUGIN_RECORD.
#include "al/al_plugin.h"

#include <windows.h>

#include <bit>
#include <cstdint>

#include <string>

#ifdef FIXTURE_NO_EXPORTS
// A DLL with exports, but not the plugin ones.
extern "C" __declspec(dllexport) int not_a_plugin() { return 0; }
#else

#ifndef FIXTURE_API
#define FIXTURE_API AL_API_VERSION
#endif
#ifndef FIXTURE_ORDER
#define FIXTURE_ORDER 0
#endif
#ifndef FIXTURE_PHASES
#define FIXTURE_PHASES 0u
#endif

namespace {
void record(const std::string& line) {
    wchar_t path[1024]{};
    if (!GetEnvironmentVariableW(L"AL_TEST_PLUGIN_RECORD", path, 1024)) return;
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const auto text = line + "\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

bool ends_with(const wchar_t* path, const wchar_t* tail) {
    const std::wstring text = path ? path : L"";
    const std::wstring end = std::wstring(L"\\") + tail;
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void*) {
#ifdef FIXTURE_NATIVE_PHASE
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    record(std::string(FIXTURE_NAME) + " phase " + std::to_string(phase));
#ifdef FIXTURE_RUNTIME
    using Check = BOOL(*)();
    const auto module = GetModuleHandleW(nullptr);
    const auto cookie = std::bit_cast<Check>(GetProcAddress(module, "al_test_cookie_unchanged"));
    const auto common = std::bit_cast<Check>(GetProcAddress(module, "al_test_common_unchanged"));
    const auto restored = std::bit_cast<Check>(GetProcAddress(module, "al_test_startup_restored"));
    record(std::string(FIXTURE_NAME) + " phase_thread " + std::to_string(GetCurrentThreadId()));
    record(cookie && cookie() && common && common() && restored && restored() ?
           "startup-state-preserved" : "startup-state-changed");
#endif
}

#ifdef FIXTURE_AFTER
const char* const after[] = {FIXTURE_AFTER, nullptr};
#define FIXTURE_AFTER_LIST after
#else
#define FIXTURE_AFTER_LIST nullptr
#endif
#ifdef FIXTURE_BEFORE
const char* const before[] = {FIXTURE_BEFORE, nullptr};
#define FIXTURE_BEFORE_LIST before
#else
#define FIXTURE_BEFORE_LIST nullptr
#endif
#ifdef FIXTURE_WRONG_EXE
const char* const exes[] = {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", nullptr};
#define FIXTURE_EXE_LIST exes
#else
#define FIXTURE_EXE_LIST nullptr
#endif
#ifdef FIXTURE_DEPENDS
#ifndef FIXTURE_DEPENDS_VERSION
#define FIXTURE_DEPENDS_VERSION nullptr
#endif
const al_dependency_t dependencies[] = {{FIXTURE_DEPENDS, FIXTURE_DEPENDS_VERSION}, {nullptr, nullptr}};
#define FIXTURE_DEPENDENCY_LIST dependencies
#else
#define FIXTURE_DEPENDENCY_LIST nullptr
#endif
#ifndef FIXTURE_INFO_SIZE
#define FIXTURE_INFO_SIZE sizeof(al_plugin_info_t)
#endif

const al_plugin_info_t info = {
    FIXTURE_INFO_SIZE, FIXTURE_API, FIXTURE_NAME, "Fixture " FIXTURE_NAME, "0.0.1", FIXTURE_PHASES,
    FIXTURE_ORDER, FIXTURE_AFTER_LIST, FIXTURE_BEFORE_LIST, FIXTURE_EXE_LIST,
    FIXTURE_PHASES ? on_phase : nullptr, FIXTURE_DEPENDENCY_LIST,
};
}  // namespace

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) {
#ifdef FIXTURE_FAULT_INFO
    return reinterpret_cast<const al_plugin_info_t*>(std::uintptr_t{1});
#else
    return &info;
#endif
}

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api,
                                                                      const al_init_t* init) {
#ifdef FIXTURE_NATIVE_INIT
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
#ifdef FIXTURE_THROW_INIT
    if (GetCurrentProcessId() != 0) throw 1;
#endif
    const bool paths = init->size == sizeof(al_init_t) && ends_with(init->folder, L"" FIXTURE_NAME) &&
                       ends_with(init->folder_real, L"" FIXTURE_NAME);
    record(std::string(FIXTURE_NAME) + " init" + (paths ? "" : " bad-paths"));
#ifdef FIXTURE_SLEEP_MS
    Sleep(FIXTURE_SLEEP_MS);
#endif
    api->log(init->self, AL_LOG_INFO, "fixture " FIXTURE_NAME " started");
#ifdef FIXTURE_FAIL
    api->set_status(init->self, AL_STATUS_DISABLED, "fixture refuses to start");
    return AL_ERROR;
#else
    return AL_OK;
#endif
}
#endif
