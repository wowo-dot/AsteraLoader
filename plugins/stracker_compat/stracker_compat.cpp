// stracker_compat: AL's replacement for Stracker's Loader.
//
// At PRE_MAIN it loads the loader.dll shim from its own folder (by full path,
// so it is the first module named loader.dll), then has it run Stracker's
// start-up. Stracker plugins in nativePC\plugins then bind their loader.dll
// import to the shim, and no dinput8.dll or loader.dll is needed in the game
// folder.

#include "al/al_plugin.h"
#include "shim.h"

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {
const al_api_t* g_api = nullptr;
al_plugin_t* g_self = nullptr;
std::wstring g_folder_real;

void say(al_log_level_t level, const std::string& text) {
    g_api->log(g_self, level, text.c_str());
}

void __cdecl forward(void*, int level, const char* line) {
    static constexpr al_log_level_t kLevels[] = {AL_LOG_DEBUG, AL_LOG_INFO, AL_LOG_WARNING, AL_LOG_ERROR};
    g_api->log(g_self, level >= 0 && level <= 3 ? kLevels[level] : AL_LOG_INFO, line);
}

// The folder with the game exe as the game sees it (under MO2: the virtual
// game folder, where nativePC, loader-config.json and loader.log live).
std::wstring game_folder() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void disable(const std::string& reason) {
    say(AL_LOG_ERROR, reason);
    g_api->set_status(g_self, AL_STATUS_DISABLED, reason.c_str());
}

void start() {
    const auto game = game_folder();
    if (game.empty()) return disable("cannot find the game folder");
    std::vector<std::string> problems;

    // Leftover Stracker's Loader files: its dinput8.dll later loads its own
    // loader.dll and the same plugins again (harmless, but two loaders), or
    // shows "loader.dll not found" if only dinput8.dll is left.
    const bool old_dinput8 = file_exists(game + L"\\dinput8.dll");
    const bool old_loader = file_exists(game + L"\\loader.dll");
    if (old_dinput8 && old_loader) {
        problems.push_back("Stracker's Loader files are still in the game folder (dinput8.dll, loader.dll); "
                           "Astera Loader replaces them, so disable or remove them");
    } else if (old_dinput8) {
        problems.push_back("a dinput8.dll is in the game folder; if it is Stracker's, remove it");
    } else if (old_loader) {
        problems.push_back("a loader.dll is in the game folder; if it is Stracker's, remove it");
    }

    // The shim must be the first module named loader.dll in the process.
    if (GetModuleHandleW(L"loader.dll")) {
        return disable("another loader.dll is already loaded, so Stracker plugins would bind to it");
    }
    const auto shim_path = g_folder_real + L"\\loader.dll";
    const HMODULE shim = LoadLibraryExW(shim_path.c_str(), nullptr, 0);
    if (!shim) return disable("cannot load its loader.dll (Windows error " + std::to_string(GetLastError()) + ")");
    const auto start_shim = reinterpret_cast<al_stracker_start_fn>(GetProcAddress(shim, AL_STRACKER_START_EXPORT));
    if (!start_shim) return disable("its loader.dll is not Astera Loader's (no " AL_STRACKER_START_EXPORT ")");

    // AL's game identity (API 1.1) already has the version; the shim then skips its search.
    const auto* identity = static_cast<const al_game_t*>(
        g_api->size > offsetof(al_api_t, get_interface) ? g_api->get_interface(g_self, AL_GAME_INTERFACE, AL_GAME_VERSION)
                                                          : nullptr);
    const al_stracker_host host{sizeof(host), AL_STRACKER_HOST_VERSION, forward, nullptr, game.c_str(),
                                  GetModuleHandleW(nullptr), identity ? identity->game_version : nullptr};
    al_stracker_result result{};
    result.size = sizeof(result);
    const auto code = start_shim(&host, &result);
    if (code == 2) return disable("its loader.dll refused the start request");
    if (code == 1) problems.push_back("Stracker start-up had already run");

    static const char* const kConfig[] = {"found", "missing (defaults used)", "not valid JSON (defaults used)"};
    say(AL_LOG_INFO, "Stracker plugins: " + std::to_string(result.plugins_found) + " found, " +
                           std::to_string(result.plugins_loaded) + " loaded, " +
                           std::to_string(result.plugins_failed) + " failed; game version " +
                           (result.version_found ? std::string(result.game_version) : std::string("not found")) +
                           "; loader-config.json " + kConfig[result.config_state <= 2 ? result.config_state : 2]);
    if (!result.plugin_loader_enabled) say(AL_LOG_INFO, "enablePluginLoader is false: no Stracker plugins loaded");

    if (result.plugins_failed) {
        problems.push_back(std::to_string(result.plugins_failed) + " Stracker plugin(s) failed to load (first: " +
                           result.first_failure + ")");
    }
    if (!result.version_found) {
        problems.push_back("the game version was not found; plugins see loader::GameVersion as \"???\"");
    }
    if (result.config_state == AL_STRACKER_CONFIG_INVALID) {
        problems.push_back("loader-config.json is not valid JSON, so Stracker's defaults are used");
    }
    if (result.config_problems) {
        problems.push_back("loader-config.json has " + std::to_string(result.config_problems) +
                           " unexpected value(s); see the log");
    }
    if (problems.empty()) {
        g_api->set_status(g_self, AL_STATUS_ACTIVE, nullptr);
        return;
    }
    std::string reason;
    for (const auto& problem : problems) reason += (reason.empty() ? "" : "; ") + problem;
    say(AL_LOG_WARNING, reason);
    g_api->set_status(g_self, AL_STATUS_DEGRADED, reason.c_str());
}

void AL_CALL on_phase(al_plugin_t*, al_phase_t phase, const void*) {
    if (phase != AL_PHASE_PRE_MAIN) return;
    try {
        start();
    } catch (...) {
        g_api->set_status(g_self, AL_STATUS_DISABLED, "unexpected error while starting Stracker plugins");
    }
}

const al_plugin_info_t kInfo = {
    sizeof(al_plugin_info_t),
    AL_API_VERSION,
    "stracker_compat",
    "Stracker's Loader compatibility",
    "0.1.0",
    AL_PHASE_BIT(AL_PHASE_PRE_MAIN),
    100,  // after native AL plugins; Stracker plugins used to load late
    nullptr,
    nullptr,
    nullptr,
    on_phase,
};
}  // namespace

extern "C" AL_PLUGIN_EXPORT const al_plugin_info_t* AL_CALL al_plugin_info(void) {
    return &kInfo;
}

extern "C" AL_PLUGIN_EXPORT al_result_t AL_CALL al_plugin_init(const al_api_t* api,
                                                                      const al_init_t* init) {
    g_api = api;
    g_self = init->self;
    try {
        g_folder_real = init->folder_real;
    } catch (...) {
        return AL_ERROR;
    }
    say(AL_LOG_INFO, "ready; Stracker plugins load at PRE_MAIN");
    return AL_OK;
}
