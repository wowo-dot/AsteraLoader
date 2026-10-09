#pragma once

// AL's crash handler. On a crash it writes one report naming the faulting
// module, the plugin that owns it (or the first plugin on the stack), the
// thread's origin and the game phase, optionally a minidump, and a notice
// for the next launch. It always passes the crash on to the filters that
// were there before; it never handles a crash itself.

#include "al/al_plugin.h"

#include <string>

namespace al::crash {

struct Options {
    std::wstring logs_folder;    // reports go to <logs>\crashes
    std::wstring settings_file;  // astera.ini ("crash_minidump = off|small|full"); may be missing
    std::string version;         // for the report, e.g. "0.1.0-dev 266f909b73d6"
    std::string exe_sha256;
};

// Installs once; later calls do nothing. Call after the game's C runtime has
// started (WIN_MAIN): the runtime replaces any top-level filter set earlier.
void install(const Options& options) noexcept;
bool installed() noexcept;
// The last phase AL dispatched, for the report.
void note_phase(al_phase_t phase) noexcept;

// The previous session's crash notice for the user, once: reads
// <logs>\crashes\last-crash.txt and renames it last-crash.seen.txt. Empty
// when there is none. Crashes while the game was exiting leave no notice.
std::wstring take_previous(const std::wstring& logs_folder);

}  // namespace al::crash
