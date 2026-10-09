// A stand-in for a Stracker plugin, built the way they are: it includes
// Stracker's header (AL's copy, loader_abi.h) and imports loader.dll. It
// logs from DllMain like many real plugins and records its load order in the
// file named by AL_TEST_STRACKER_RECORD.
#include "loader_abi.h"

#include <windows.h>

#include <string>

namespace {
void record(const std::string& line) {
    char path[1024]{};
    if (!GetEnvironmentVariableA("AL_TEST_STRACKER_RECORD", path, sizeof(path))) return;
    const HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const auto text = line + "\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

std::string own_name(HMODULE module) {
    char path[MAX_PATH]{};
    GetModuleFileNameA(module, path, MAX_PATH);
    const std::string text = path;
    return text.substr(text.find_last_of("\\/") + 1);
}
}  // namespace

// Where this plugin's imports point, for the test to compare with the shim.
extern "C" __declspec(dllexport) const loader::LogLevel* fixture_min_level() {
    return &loader::MinLogLevel;
}
extern "C" __declspec(dllexport) const char* fixture_game_version() {
    return loader::GameVersion;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        const auto name = own_name(module);
        record(name);
        loader::LOG(loader::ERR) << "fixture " << name << " sees game version " << loader::GameVersion;
        loader::LOG(loader::DEBUG) << "fixture debug line " << 42;
    }
    return TRUE;
}
