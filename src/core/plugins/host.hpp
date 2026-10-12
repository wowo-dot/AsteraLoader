#pragma once

#include "al/al_plugin.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace al::graphics {
class Hub;
}
namespace al::game_hooks {
class Registry;
}
namespace al::patch {
class Registry;
}
namespace al::frame {
class Clock;
}
namespace al::input { class Service; }

namespace al::plugins {

struct HostOptions {
    std::wstring plugins_root;  // <game>\AsteraLoader\plugins as the game sees it
    std::wstring logs_folder;   // AL's log folder; plugin logs go in <logs>\plugins
    std::wstring exe_sha256;    // the running exe, lower-case hex; empty if unknown
    // API 1.1. Empty means a folder beside logs_folder (<AsteraLoader>\data, <AsteraLoader>\cache)
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
    al_status_t status = AL_STATUS_DISABLED;
    std::string reason;
    bool initialised = false;  // al_plugin_init was called; stays loaded
    std::wstring folder;
    std::wstring folder_real;
    // Start-up cost on the calling thread, in microseconds.
    std::uint64_t load_us = 0;                               // LoadLibrary and the info check
    std::uint64_t init_us = 0;                               // al_plugin_init
    std::array<std::uint64_t, AL_PHASE_COUNT> phase_us{};  // on_phase, summed per phase
};

struct HostState;  // services and published interfaces, shared by all plugins

// Loads and runs AL plugins. It lives until the process exits: initialised
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
    void dispatch(al_phase_t phase, const void* data);

    // Initialised plugins in load order, then the ones that did not run.
    std::vector<PluginReport> report() const;

    // What the user should be told once, now: a crash in the previous session
    // (crash handler notice). Valid after load_all.
    std::vector<std::wstring> notices() const;

    // The "al.d3d12" service, for the hook layer. Registration closes when
    // dispatch(PRE_MAIN) returns; dispatch(DEVICE_CREATED) hands it the device.
    // Valid after load_all.
    ::al::graphics::Hub& graphics();

    // The "al.hooks" service, for the hook layer: hand it the installer
    // after PRE_MAIN with set_installer. Valid after load_all.
    ::al::game_hooks::Registry& game_hooks();

    // The "al.patch" service (1.8), for the hook layer: hand it the writer
    // after PRE_MAIN with set_writer, as with game_hooks. dispatch(SHUTDOWN)
    // puts back its DATA changes. Valid after load_all.
    ::al::patch::Registry& patches();

    // The "al.frame" service, for the frame layer: begin_frame/end_frame
    // around each game frame, last_frame when the game leaves its loop,
    // set_unavailable if the layer cannot be installed. dispatch(SHUTDOWN)
    // closes its task queue. Valid after load_all.
    ::al::frame::Clock& frame_clock();

    // Dispatches AL_PHASE_EXITING once: the first call does, later ones (from
    // the backup path or the other thread) return at once. Returns true for
    // the call that dispatched it. Any thread.
    bool raise_exiting();
    // Dispatches AL_PHASE_GAME_READY once, and never after EXITING. Returns
    // true for the call that dispatched it.
    bool raise_game_ready(const al_phase_game_ready_t& payload);
    // Whether a running plugin subscribed to `phase`.
    bool subscribed(al_phase_t phase) const;
    // Reuse the cached identity rather than scanning the image again.
    const al_game_t* game_identity();
    // Whether an initial plugin asked for frames or the EXITING phase.
    bool frame_requested() const;
    ::al::input::Service& input_service();

private:
    void close_graphics();

    std::unique_ptr<HostState> state_;
    std::vector<std::unique_ptr<al_plugin_t>> plugins_;
};

}  // namespace al::plugins
