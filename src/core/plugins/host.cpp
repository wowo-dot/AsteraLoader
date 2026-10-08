#include "core/plugins/host.hpp"

#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/pattern.hpp"
#include "common/windows.hpp"
#include "core/plugins/discovery.hpp"
#include "core/plugins/guard.hpp"
#include "core/plugins/load_order.hpp"
#include "core/plugins/manifest.hpp"
#include "core/plugins/services.hpp"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>

namespace cmfm::plugins {
namespace {
// A plugin's own log file, <logs>\plugins\<name>.log. It is created on the
// first line, keeps the previous run as <name>.previous.log, and stops at a
// size limit so a chatty plugin cannot fill the disk.
class PluginLog {
public:
    explicit PluginLog(std::wstring path) : path_(std::move(path)) {}
    ~PluginLog() {
        if (file_) CloseHandle(file_);
    }
    PluginLog(const PluginLog&) = delete;
    PluginLog& operator=(const PluginLog&) = delete;

    void write(std::string_view level, std::string_view message) {
        std::lock_guard lock(mutex_);
        if (stopped_ || (!file_ && !open())) return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char stamp[32]{};
        const int length = std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ", unsigned{now.wHour},
                                         unsigned{now.wMinute}, unsigned{now.wSecond}, unsigned{now.wMilliseconds});
        std::string line(stamp, static_cast<std::size_t>(length > 0 ? length : 0));
        line.append(level).append(" ").append(message).append("\r\n");
        if (written_ + line.size() > kLimit) {
            line = "log limit reached (8 MiB); further lines are dropped\r\n";
            stopped_ = true;
        }
        DWORD done = 0;
        if (!WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &done, nullptr)) stopped_ = true;
        written_ += done;
    }

private:
    bool open() {
        const std::wstring folder(directory_of(path_));
        CreateDirectoryW(std::wstring(directory_of(folder)).c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        const auto previous = path_.substr(0, path_.size() - 4) + L".previous.log";
        MoveFileExW(path_.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        const HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            stopped_ = true;
            return false;
        }
        file_ = file;
        return true;
    }

    static constexpr std::uint64_t kLimit = 8ull << 20;
    std::wstring path_;
    std::mutex mutex_;
    HANDLE file_ = nullptr;
    std::uint64_t written_ = 0;
    bool stopped_ = false;
};

std::string_view level_name(cmfm_log_level_t level) {
    switch (level) {
    case CMFM_LOG_DEBUG: return "DEBUG";
    case CMFM_LOG_INFO: return "INFO";
    case CMFM_LOG_WARNING: return "WARN";
    case CMFM_LOG_ERROR: return "ERROR";
    default: return "LOG";
    }
}

std::string_view status_name(cmfm_status_t status) {
    switch (status) {
    case CMFM_STATUS_ACTIVE: return "active";
    case CMFM_STATUS_DEGRADED: return "degraded";
    default: return "disabled";
    }
}

// Plugin text is capped so a missing terminator cannot run on without end.
std::string plugin_text(const char* text, std::size_t limit) {
    return text ? std::string(text, strnlen(text, limit)) : std::string();
}

std::uint64_t microseconds_since(const LARGE_INTEGER& start) {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? static_cast<std::uint64_t>(now.QuadPart - start.QuadPart) * 1000000 /
                                    static_cast<std::uint64_t>(frequency.QuadPart)
                              : 0;
}

// An interface version satisfies a request with the same major version and
// at most the same minor version.
bool compatible(std::uint32_t have, std::uint32_t want) {
    return CMFM_VERSION_MAJOR(have) == CMFM_VERSION_MAJOR(want) && CMFM_VERSION_MINOR(have) >= CMFM_VERSION_MINOR(want);
}

std::wstring scan_cache_file(const HostOptions& options) {
    if (options.exe_sha256.empty()) return {};
    const auto folder = options.cache_folder.empty()
                            ? join_path(directory_of(options.logs_folder), L"cache")
                            : options.cache_folder;
    return join_path(folder, L"scan." + options.exe_sha256 + L".txt");
}
}  // namespace

// Services and interfaces every plugin shares; lives as long as the Host.
struct HostState {
    explicit HostState(const HostOptions& options)
        : scanner(GetModuleHandleW(nullptr), scan_cache_file(options)),
          game(GetModuleHandleW(nullptr), options.exe_sha256, options.game_folder, &scanner) {}

    Scanner scanner;  // before game, which uses it
    GameService game;
    std::mutex mutex;  // guards interfaces
    struct Published {
        cmfm_plugin_t* owner;
        std::uint32_t version;
        const void* table;
    };
    std::map<std::string, Published> interfaces;
};

}  // namespace cmfm::plugins

// The opaque cmfm_plugin_t of the C API: one per entry in the plugins folder.
struct cmfm_plugin {
    std::wstring name;
    std::wstring folder;
    std::wstring folder_real;
    std::wstring data_folder;
    cmfm::plugins::HostState* state = nullptr;
    HMODULE module = nullptr;
    cmfm_plugin_init_fn init = nullptr;
    cmfm::plugins::Manifest manifest;
    bool initialised = false;
    cmfm_init_t init_block{};  // handed to the plugin; must outlive it
    std::unique_ptr<cmfm::plugins::PluginLog> log;
    std::uint64_t load_us = 0;
    std::uint64_t init_us = 0;
    std::array<std::uint64_t, CMFM_PHASE_COUNT> phase_us{};
    mutable std::mutex mutex;  // guards status and reason
    cmfm_status_t status = CMFM_STATUS_DISABLED;
    std::string reason;
};

namespace cmfm::plugins {
namespace {
void note(cmfm_plugin_t& plugin, std::string_view level, std::string_view text) {
    if (plugin.log) plugin.log->write(level, text);
}

void CMFM_CALL api_log(cmfm_plugin_t* self, cmfm_log_level_t level, const char* message) noexcept {
    if (!self || !self->log) return;
    try {
        self->log->write(level_name(level), message ? plugin_text(message, 16384) : "(null)");
    } catch (...) { /* Logging must never throw into a plugin. */ }
}

void CMFM_CALL api_set_status(cmfm_plugin_t* self, cmfm_status_t status, const char* reason) noexcept {
    if (!self || status > CMFM_STATUS_DISABLED) return;
    try {
        auto text = plugin_text(reason, 1024);
        note(*self, "STATUS", std::string(status_name(status)) + (text.empty() ? "" : ": " + text));
        std::lock_guard lock(self->mutex);
        self->status = status;
        self->reason = std::move(text);
    } catch (...) { /* Never throw into a plugin. */ }
}

std::uint32_t CMFM_CALL scanner_find(cmfm_plugin_t* self, const char* pattern, std::uint32_t where, void** matches,
                                     std::uint32_t capacity) noexcept {
    if (!self || !self->state || !pattern) return CMFM_SCAN_INVALID;
    try {
        const auto text = plugin_text(pattern, 2048);
        const auto parsed = parse_pattern(text);
        const auto shown = "\"" + text.substr(0, 200) + "\"";
        if (!parsed || (where != CMFM_SCAN_CODE && where != CMFM_SCAN_IMAGE)) {
            note(*self, "WARN", "scan " + shown + ": not a pattern or scope CMFM can read");
            return CMFM_SCAN_INVALID;
        }
        const auto result = self->state->scanner.find(*parsed, where);
        const auto count = static_cast<std::uint32_t>(result.matches.size());
        for (std::uint32_t k = 0; matches && k < std::min(count, capacity); ++k) {
            matches[k] = const_cast<void*>(result.matches[k]);
        }
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), result.cached ? "from the cache, %.2f ms" : "searched in %.1f ms",
                      result.milliseconds);
        note(*self, "DEBUG", "scan " + shown + (where == CMFM_SCAN_CODE ? " in code: " : " in image: ") +
                                 std::to_string(count) + " match(es), " + detail);
        return count;
    } catch (...) {
        return CMFM_SCAN_INVALID;
    }
}

void* CMFM_CALL scanner_rip_target(const void* instruction, std::uint32_t offset, std::uint32_t length) noexcept {
    return rip_target(instruction, offset, length);
}

const cmfm_scanner_t kScanner{sizeof(cmfm_scanner_t), CMFM_SCANNER_VERSION, scanner_find, scanner_rip_target};

const void* CMFM_CALL api_get_interface(cmfm_plugin_t* self, const char* name, std::uint32_t version) noexcept {
    if (!self || !self->state || !name) return nullptr;
    try {
        const auto wanted = plugin_text(name, 129);
        auto& state = *self->state;
        if (wanted == CMFM_GAME_INTERFACE) return compatible(CMFM_GAME_VERSION, version) ? state.game.table() : nullptr;
        if (wanted == CMFM_SCANNER_INTERFACE) return compatible(CMFM_SCANNER_VERSION, version) ? &kScanner : nullptr;
        std::lock_guard lock(state.mutex);
        const auto found = state.interfaces.find(wanted);
        if (found == state.interfaces.end() || !compatible(found->second.version, version)) return nullptr;
        std::lock_guard owner(found->second.owner->mutex);
        return found->second.owner->status == CMFM_STATUS_DISABLED ? nullptr : found->second.table;
    } catch (...) {
        return nullptr;
    }
}

cmfm_result_t CMFM_CALL api_publish_interface(cmfm_plugin_t* self, const char* name, std::uint32_t version,
                                              const void* table) noexcept {
    if (!self || !self->state) return CMFM_ERROR;
    try {
        const auto text = plugin_text(name, 129);
        const auto prefix = self->manifest.name + ".";
        std::string problem;
        if (!table) {
            problem = "no table";
        } else if (text.size() > 128 || text.size() <= prefix.size() || text.compare(0, prefix.size(), prefix) != 0) {
            problem = "the name must start with \"" + prefix + "\" (128 characters at most)";
        } else if (!std::all_of(text.begin(), text.end(), [](char c) {
                       return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                   })) {
            problem = "the name may use a-z, 0-9, '_', '-' and '.' only";
        } else {
            std::lock_guard lock(self->state->mutex);
            if (!self->state->interfaces.emplace(text, HostState::Published{self, version, table}).second) {
                problem = "the name is taken";
            }
        }
        const auto shown = text + " " + std::to_string(CMFM_VERSION_MAJOR(version)) + "." +
                           std::to_string(CMFM_VERSION_MINOR(version));
        note(*self, problem.empty() ? "INFO" : "WARN",
             problem.empty() ? "published interface " + shown : "cannot publish interface " + shown + ": " + problem);
        return problem.empty() ? CMFM_OK : CMFM_ERROR;
    } catch (...) {
        return CMFM_ERROR;
    }
}

const cmfm_api_t kApi{sizeof(cmfm_api_t), CMFM_API_VERSION, api_log, api_set_status, api_get_interface,
                      api_publish_interface};

void refuse(cmfm_plugin_t& plugin, std::string reason) {
    if (plugin.module && !plugin.initialised) {
        FreeLibrary(plugin.module);
        plugin.module = nullptr;
    }
    std::lock_guard lock(plugin.mutex);
    plugin.status = CMFM_STATUS_DISABLED;
    plugin.reason = std::move(reason);
}

// Loads the plugin DLL by its real path, resolved through a file handle, so
// loading does not depend on how MO2 maps the folder. The plugin's folder is
// searched for its own DLLs; the process DLL search path is left alone.
bool load(cmfm_plugin_t& plugin, const Candidate& candidate, std::wstring_view exe_sha256, std::string& reason) {
    // Do not bind a CMFM plugin to a DLL name already owned by the process.
    // Reserve proxy/framework names even if that module has not loaded yet.
    const auto base_name = candidate.name + L".dll";
    for (const auto reserved : {L"cmfm", L"loader", L"dinput8", L"winmm", L"dxgi", L"d3d12",
                               L"kernel32", L"kernelbase", L"ntdll", L"steam_api64", L"steamclient64", L"usvfs_x64"}) {
        if (iequals(candidate.name, reserved)) {
            reason = "its DLL name is reserved for the game, Windows or another loader";
            return false;
        }
    }
    if (GetModuleHandleW(base_name.c_str())) {
        reason = "a DLL with that name is already loaded; refusing a module-name collision";
        return false;
    }
    std::wstring dll_real;
    {
        Handle file(CreateFileW(candidate.dll.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            reason = "cannot open its DLL: " + to_utf8(win32_error_text(GetLastError()));
            return false;
        }
        dll_real = strip_extended_prefix(final_path(file.get()));
    }
    if (dll_real.empty()) {
        reason = "cannot resolve its DLL's real path: " + to_utf8(win32_error_text(GetLastError()));
        return false;
    }
    plugin.folder_real = std::wstring(directory_of(dll_real));
    // A damaged DLL would make Windows show a "Bad Image" box and wait for a
    // click; the refusal is reported instead. Only this thread's error mode
    // changes, and only for the load.
    DWORD previous_mode = 0;
    const bool quiet = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    plugin.module = LoadLibraryExW(dll_real.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD load_error = GetLastError();
    if (quiet) SetThreadErrorMode(previous_mode, nullptr);
    if (!plugin.module) {
        reason = "Windows could not load it: " + to_utf8(win32_error_text(load_error));
        return false;
    }
    const auto info = std::bit_cast<cmfm_plugin_info_fn>(GetProcAddress(plugin.module, CMFM_PLUGIN_INFO_EXPORT));
    plugin.init = std::bit_cast<cmfm_plugin_init_fn>(GetProcAddress(plugin.module, CMFM_PLUGIN_INIT_EXPORT));
    if (!info || !plugin.init) {
        reason = "it is not a CMFM plugin (no cmfm_plugin_info and cmfm_plugin_init exports)";
        return false;
    }
    unsigned long fault = 0;
    const auto valid = guarded_manifest(info, plugin.name, exe_sha256, plugin.manifest, reason, fault);
    if (fault) reason = "cmfm_plugin_info or its metadata raised exception " + std::to_string(fault);
    return valid;
}

// The plugin's data folder, created on the way; null if that fails.
const wchar_t* make_data_folder(const std::wstring& folder) {
    CreateDirectoryW(std::wstring(directory_of(folder)).c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? folder.c_str() : nullptr;
}

void initialise(cmfm_plugin_t& plugin) {
    // Only plugins built for 1.1 or later can see data_folder; others get no folder.
    const bool sees_data_folder = plugin.manifest.api_version >= CMFM_MAKE_VERSION(1, 1);
    plugin.init_block = {sizeof(cmfm_init_t), 0, &plugin, plugin.folder.c_str(), plugin.folder_real.c_str(),
                         sees_data_folder ? make_data_folder(plugin.data_folder) : nullptr};
    {
        std::lock_guard lock(plugin.mutex);
        plugin.status = CMFM_STATUS_ACTIVE;
        plugin.reason.clear();
    }
    // From here on CMFM never unloads the plugin, whatever init returns.
    plugin.initialised = true;
    cmfm_result_t result = CMFM_ERROR;
    unsigned long fault = 0;
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    const bool returned = guarded_init(plugin.init, &kApi, &plugin.init_block, result, fault);
    plugin.init_us = microseconds_since(start);
    if (!returned) {
        std::lock_guard lock(plugin.mutex);
        plugin.status = CMFM_STATUS_DISABLED;
        plugin.reason = "cmfm_plugin_init raised exception " + std::to_string(fault);
        return;
    }
    if (result == CMFM_OK) return;
    std::lock_guard lock(plugin.mutex);
    plugin.status = CMFM_STATUS_DISABLED;
    if (plugin.reason.empty()) plugin.reason = "cmfm_plugin_init returned " + std::to_string(result);
}

bool running(const cmfm_plugin_t& plugin) {
    std::lock_guard lock(plugin.mutex);
    return plugin.initialised && plugin.status != CMFM_STATUS_DISABLED;
}
}  // namespace

Host::Host() = default;
Host::~Host() = default;

void Host::load_all(const HostOptions& options) {
    state_ = std::make_unique<HostState>(options);
    // Plain paths for plugins: CMFM's own folders may carry the long-path
    // prefix of a resolved file path (\\?\D:\...), which plugins should not see.
    const auto data_root = strip_extended_prefix(
        options.data_root.empty() ? join_path(directory_of(options.logs_folder), L"data") : options.data_root);
    const auto found = discover(options.plugins_root);
    const auto make = [&](const std::wstring& name) {
        auto plugin = std::make_unique<cmfm_plugin_t>();
        plugin->name = name;
        plugin->folder = join_path(options.plugins_root, name);
        plugin->data_folder = join_path(data_root, name);
        plugin->state = state_.get();
        plugin->log = std::make_unique<PluginLog>(join_path(join_path(options.logs_folder, L"plugins"),
                                                            name + L".log"));
        return plugin;
    };

    std::vector<std::unique_ptr<cmfm_plugin_t>> loaded, not_loaded;
    for (const auto& candidate : found.candidates) {
        auto plugin = make(candidate.name);
        std::string reason;
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        const bool ok = load(*plugin, candidate, options.exe_sha256, reason);
        plugin->load_us = microseconds_since(start);
        if (ok) {
            loaded.push_back(std::move(plugin));
        } else {
            refuse(*plugin, std::move(reason));
            not_loaded.push_back(std::move(plugin));
        }
    }
    for (const auto& skipped : found.skipped) {
        auto plugin = make(skipped.entry);
        refuse(*plugin, skipped.reason);
        not_loaded.push_back(std::move(plugin));
    }

    // Dependencies first: a missing, refused or too old required plugin
    // refuses its dependants before anything starts.
    std::vector<Manifest> manifests;
    for (const auto& plugin : loaded) manifests.push_back(plugin->manifest);
    std::vector<Unavailable> unavailable;
    for (const auto& plugin : not_loaded) unavailable.push_back({to_utf8(plugin->name), plugin->reason});
    std::vector<bool> refused(loaded.size(), false);
    for (const auto& refusal : check_dependencies(manifests, unavailable)) {
        refuse(*loaded[refusal.index], refusal.reason);
        refused[refusal.index] = true;
    }
    std::vector<std::size_t> remaining;
    std::vector<Manifest> ordered;
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        if (refused[i]) continue;
        remaining.push_back(i);
        ordered.push_back(manifests[i]);
    }

    // Start in order. Dependencies come earlier in the order, so one that is
    // not running by now failed to start or was refused for its order.
    const auto order = resolve_load_order(ordered);
    std::map<std::string, const cmfm_plugin_t*> started;
    for (const auto k : order.sequence) {
        auto& plugin = *loaded[remaining[k]];
        std::string missing;
        for (const auto& dependency : plugin.manifest.dependencies) {
            if (dependency.name == plugin.manifest.name) continue;
            const auto found_dependency = started.find(dependency.name);
            if (found_dependency == started.end() || !running(*found_dependency->second)) {
                missing = dependency.name;
                break;
            }
        }
        if (missing.empty()) {
            initialise(plugin);
        } else {
            refuse(plugin, "it needs " + missing + ", which did not start");
        }
        started[plugin.manifest.name] = &plugin;
        plugins_.push_back(std::move(loaded[remaining[k]]));
    }
    for (const auto& refusal : order.refused) {
        refuse(*loaded[remaining[refusal.index]], refusal.reason);
        plugins_.push_back(std::move(loaded[remaining[refusal.index]]));
    }
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        if (refused[i]) plugins_.push_back(std::move(loaded[i]));
    }
    for (auto& plugin : not_loaded) plugins_.push_back(std::move(plugin));
}

void Host::dispatch(cmfm_phase_t phase, const void* data) {
    if (phase >= CMFM_PHASE_COUNT) return;
    for (const auto& plugin : plugins_) {
        if (!plugin->initialised || !(plugin->manifest.phases & CMFM_PHASE_BIT(phase))) continue;
        {
            std::lock_guard lock(plugin->mutex);
            if (plugin->status == CMFM_STATUS_DISABLED) continue;
        }
        unsigned long fault = 0;
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        const bool returned = guarded_phase(plugin->manifest.on_phase, plugin.get(), phase, data, fault);
        plugin->phase_us[phase] += microseconds_since(start);
        if (!returned) {
            std::lock_guard lock(plugin->mutex);
            plugin->status = CMFM_STATUS_DISABLED;
            plugin->reason = "on_phase raised exception " + std::to_string(fault);
        }
    }
}

std::vector<PluginReport> Host::report() const {
    std::vector<PluginReport> lines;
    for (const auto& plugin : plugins_) {
        PluginReport line;
        line.name = plugin->name;
        line.display_name = plugin->manifest.display_name;
        line.version = plugin->manifest.version;
        line.initialised = plugin->initialised;
        line.folder = plugin->folder;
        line.folder_real = plugin->folder_real;
        line.load_us = plugin->load_us;
        line.init_us = plugin->init_us;
        line.phase_us = plugin->phase_us;
        {
            std::lock_guard lock(plugin->mutex);
            line.status = plugin->status;
            line.reason = plugin->reason;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace cmfm::plugins
