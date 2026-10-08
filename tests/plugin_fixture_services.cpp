// Fixture plugins for API 1.1 services, built as "provider" or "consumer".
// They record what they see in the file named by CMFM_TEST_PLUGIN_RECORD.
#include "cmfm/cmfm_plugin.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {
void record(const std::string& line) {
    wchar_t path[1024]{};
    if (!GetEnvironmentVariableW(L"CMFM_TEST_PLUGIN_RECORD", path, 1024)) return;
    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const auto text = line + "\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

// The table "provider" publishes as "provider.math" 1.2.
struct math_t {
    uint32_t size;
    uint32_t version;
    int(CMFM_CALL* add)(int a, int b);
};

#ifdef FIXTURE_PROVIDER
int CMFM_CALL add(int a, int b) {
    return a + b;
}
const math_t kMath{sizeof(math_t), CMFM_MAKE_VERSION(1, 2), add};
const cmfm_dependency_t* const kDependencies = nullptr;
constexpr const char* kName = "provider";
constexpr const char* kVersion = "0.3.1";
#else
const cmfm_dependency_t kNeeds[] = {{"provider", "0.3"}, {nullptr, nullptr}};
const cmfm_dependency_t* const kDependencies = kNeeds;
constexpr const char* kName = "consumer";
constexpr const char* kVersion = "1.0";
#endif

const cmfm_plugin_info_t kInfo = {
    sizeof(cmfm_plugin_info_t), CMFM_API_VERSION, kName, nullptr, kVersion, 0, 0, nullptr, nullptr, nullptr,
    nullptr, kDependencies,
};

#ifndef FIXTURE_PROVIDER
std::string hex(const void* address) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%p", address);
    return text;
}
#endif
}  // namespace

extern "C" CMFM_PLUGIN_EXPORT const cmfm_plugin_info_t* CMFM_CALL cmfm_plugin_info(void) {
    return &kInfo;
}

extern "C" CMFM_PLUGIN_EXPORT cmfm_result_t CMFM_CALL cmfm_plugin_init(const cmfm_api_t* api,
                                                                      const cmfm_init_t* init) {
    const auto self = init->self;
#ifdef FIXTURE_PROVIDER
    record(api->publish_interface(self, "provider.math", kMath.version, &kMath) == CMFM_OK ? "provider published"
                                                                                           : "provider not published");
    record(api->publish_interface(self, "provider.math", kMath.version, &kMath) == CMFM_ERROR ? "provider taken"
                                                                                              : "provider twice");
    record(api->publish_interface(self, "other.math", kMath.version, &kMath) == CMFM_ERROR ? "provider foreign refused"
                                                                                           : "provider foreign");
    record(api->publish_interface(self, "provider.Math", kMath.version, &kMath) == CMFM_ERROR ? "provider case refused"
                                                                                              : "provider case");
#else
    const auto* math = static_cast<const math_t*>(api->get_interface(self, "provider.math", CMFM_MAKE_VERSION(1, 0)));
    record(math && math->add(2, 3) == 5 ? "consumer math 5" : "consumer math missing");
    record(!api->get_interface(self, "provider.math", CMFM_MAKE_VERSION(1, 3)) &&
                   !api->get_interface(self, "provider.math", CMFM_MAKE_VERSION(2, 0)) &&
                   !api->get_interface(self, "nobody.math", CMFM_MAKE_VERSION(1, 0))
               ? "consumer mismatches null"
               : "consumer mismatch returned");

    const auto* game = static_cast<const cmfm_game_t*>(api->get_interface(self, CMFM_GAME_INTERFACE, CMFM_GAME_VERSION));
    record(game ? "consumer game " + std::string(game->exe_sha256 ? game->exe_sha256 : "none") + " graphics " +
                      std::to_string(game->graphics)
                : "consumer game missing");

    const auto* scanner =
        static_cast<const cmfm_scanner_t*>(api->get_interface(self, CMFM_SCANNER_INTERFACE, CMFM_SCANNER_VERSION));
    char pattern[256]{};
    GetEnvironmentVariableA("CMFM_TEST_SCAN_PATTERN", pattern, sizeof(pattern));
    void* matches[4]{};
    const auto count = scanner ? scanner->find(self, pattern, CMFM_SCAN_IMAGE, matches, 4) : 0;
    record("consumer scan " + std::to_string(count) + " " + hex(matches[0]));
    record(scanner && scanner->find(self, "zz", CMFM_SCAN_CODE, nullptr, 0) == CMFM_SCAN_INVALID &&
                   scanner->find(self, "90 90", 7, nullptr, 0) == CMFM_SCAN_INVALID
               ? "consumer invalid refused"
               : "consumer invalid accepted");

    bool wrote = false;
    if (init->data_folder) {
        const auto path = std::wstring(init->data_folder) + L"\\consumer.txt";
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        wrote = file != INVALID_HANDLE_VALUE;
        if (wrote) CloseHandle(file);
    }
    record(wrote ? "consumer data written" : "consumer no data folder");
#endif
    return CMFM_OK;
}
