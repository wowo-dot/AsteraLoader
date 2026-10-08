#include "core/plugins/host.hpp"

#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"
#include "core/plugins/discovery.hpp"
#include "core/plugins/guard.hpp"
#include "core/plugins/load_order.hpp"
#include "core/plugins/manifest.hpp"

#include <windows.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
}  // namespace
}  // namespace cmfm::plugins

// The opaque cmfm_plugin_t of the C API: one per entry in the plugins folder.
struct cmfm_plugin {
    std::wstring name;
    std::wstring folder;
    std::wstring folder_real;
    HMODULE module = nullptr;
    cmfm_plugin_init_fn init = nullptr;
    cmfm::plugins::Manifest manifest;
    bool initialised = false;
    cmfm_init_t init_block{};  // handed to the plugin; must outlive it
    std::unique_ptr<cmfm::plugins::PluginLog> log;
    mutable std::mutex mutex;  // guards status and reason
    cmfm_status_t status = CMFM_STATUS_DISABLED;
    std::string reason;
};

namespace cmfm::plugins {
namespace {
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
        if (self->log) {
            self->log->write("STATUS", std::string(status_name(status)) + (text.empty() ? "" : ": " + text));
        }
        std::lock_guard lock(self->mutex);
        self->status = status;
        self->reason = std::move(text);
    } catch (...) { /* Never throw into a plugin. */ }
}

const cmfm_api_t kApi{sizeof(cmfm_api_t), CMFM_API_VERSION, api_log, api_set_status};

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

void initialise(cmfm_plugin_t& plugin) {
    plugin.init_block = {sizeof(cmfm_init_t), 0, &plugin, plugin.folder.c_str(), plugin.folder_real.c_str()};
    {
        std::lock_guard lock(plugin.mutex);
        plugin.status = CMFM_STATUS_ACTIVE;
        plugin.reason.clear();
    }
    // From here on CMFM never unloads the plugin, whatever init returns.
    plugin.initialised = true;
    cmfm_result_t result = CMFM_ERROR;
    unsigned long fault = 0;
    if (!guarded_init(plugin.init, &kApi, &plugin.init_block, result, fault)) {
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
}  // namespace

Host::Host() = default;
Host::~Host() = default;

void Host::load_all(const HostOptions& options) {
    const auto found = discover(options.plugins_root);
    const auto make = [&options](const std::wstring& name) {
        auto plugin = std::make_unique<cmfm_plugin_t>();
        plugin->name = name;
        plugin->folder = join_path(options.plugins_root, name);
        plugin->log = std::make_unique<PluginLog>(join_path(join_path(options.logs_folder, L"plugins"),
                                                            name + L".log"));
        return plugin;
    };

    std::vector<std::unique_ptr<cmfm_plugin_t>> loaded, not_loaded;
    for (const auto& candidate : found.candidates) {
        auto plugin = make(candidate.name);
        std::string reason;
        if (load(*plugin, candidate, options.exe_sha256, reason)) {
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

    std::vector<Manifest> manifests;
    for (const auto& plugin : loaded) manifests.push_back(plugin->manifest);
    const auto order = resolve_load_order(manifests);
    for (const auto i : order.sequence) {
        initialise(*loaded[i]);
        plugins_.push_back(std::move(loaded[i]));
    }
    for (const auto& refusal : order.refused) {
        refuse(*loaded[refusal.index], refusal.reason);
        plugins_.push_back(std::move(loaded[refusal.index]));
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
        if (!guarded_phase(plugin->manifest.on_phase, plugin.get(), phase, data, fault)) {
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
