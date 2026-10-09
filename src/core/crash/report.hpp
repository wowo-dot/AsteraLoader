#pragma once

// Pure pieces of AL's crash handler: who owns a module, what an exception
// code means, and the crash settings in AsteraLoader.ini. Everything here works
// without the heap, so the handler can use it while the process is failing.

#include <cstdint>
#include <string_view>

namespace al::crash {

enum class OwnerKind {
    unknown,
    game,             // the game exe
    core,             // AsteraLoader.dll
    plugin,           // an AL plugin, or a DLL it ships: <...>\AsteraLoader\plugins\<name>\...
    stracker_plugin,  // a Stracker plugin: <...>\nativePC\plugins\<file>
    windows,          // System32, SysWOW64, WinSxS
    other,            // anything else: drivers, overlays, other loaders
};

struct Owner {
    OwnerKind kind = OwnerKind::unknown;
    std::wstring_view name;  // the plugin's folder name, or the module's file name
};

// `path` is a module path in any form: a drive path or a \Device\... path,
// MO2's virtual game folder or the real mod folder (both keep the plugin
// layout). `is_core` / `is_game` come from comparing the module's base.
Owner classify(std::wstring_view path, bool is_core, bool is_game) noexcept;

// "plugin", "Stracker plugin", "Astera Loader", "the game", "Windows", "module".
const char* owner_label(OwnerKind kind) noexcept;
// A plugin or Stracker plugin: something the user installed and can remove.
bool removable(OwnerKind kind) noexcept;

// Exceptions that end the process unless something handles them.
bool fatal(unsigned long code) noexcept;
// "access violation" and the like; "exception" for codes it does not know.
const char* exception_name(unsigned long code) noexcept;

enum class Minidump { off, small, full };
// The `crash_minidump = off | small | full` line of AsteraLoader.ini; off when absent
// or unreadable.
Minidump minidump_setting(std::string_view ini_text) noexcept;

}  // namespace al::crash
