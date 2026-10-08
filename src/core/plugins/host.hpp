#pragma once

#include "cmfm/cmfm_plugin.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cmfm::plugins {

struct HostOptions {
    std::wstring plugins_root;  // <game>\cmfm\plugins as the game sees it
    std::wstring logs_folder;   // CMFM's log folder; plugin logs go in <logs>\plugins
    std::wstring exe_sha256;    // the running exe, lower-case hex; empty if unknown
    // API 1.1. Empty means a folder beside logs_folder (<cmfm>\data, <cmfm>\cache)
    // or, for game_folder, the exe's folder.
    std::wstring data_root;     // plugin data folders: <data_root>\<name>
    std::wstring cache_folder;  // the scanner's cache, one file per exe hash
    std::wstring game_folder;   // where graphics_option.ini is read
};

// One line of the plugin status the user sees.
struct PluginReport {
    std::wstring name;  // folder or file name in the plugins folder
    std::string display_name;
    std::string version;
    cmfm_status_t status = CMFM_STATUS_DISABLED;
    std::string reason;
    bool initialised = false;  // cmfm_plugin_init was called; stays loaded
    std::wstring folder;
    std::wstring folder_real;
    // Start-up cost on the calling thread, in microseconds.
    std::uint64_t load_us = 0;                               // LoadLibrary and the info check
    std::uint64_t init_us = 0;                               // cmfm_plugin_init
    std::array<std::uint64_t, CMFM_PHASE_COUNT> phase_us{};  // on_phase, summed per phase
};

struct HostState;  // services and published interfaces, shared by all plugins

// Loads and runs CMFM plugins. It lives until the process exits: initialised
// plugins hold pointers into it and are never unloaded.
class Host {
public:
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Finds, loads, checks, orders and initialises every plugin under
    // options.plugins_root. A plugin that cannot run is unloaded again if
    // its init was never called, and reported with the reason. Call once.
    void load_all(const HostOptions& options);

    // Calls on_phase of every running plugin that asked for `phase`, in load
    // order, on the calling thread.
    void dispatch(cmfm_phase_t phase, const void* data);

    // Initialised plugins in load order, then the ones that did not run.
    std::vector<PluginReport> report() const;

private:
    std::unique_ptr<HostState> state_;
    std::vector<std::unique_ptr<cmfm_plugin_t>> plugins_;
};

}  // namespace cmfm::plugins
