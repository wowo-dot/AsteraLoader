#pragma once

// Pure logic of the Stracker's Loader replacement, kept out of the DLLs so it
// can be unit-tested: loader-config.json and the order Stracker loads
// nativePC\plugins in. The game-version pattern lives in
// src/common/game_version.hpp, shared with AL's game-identity service.

#include <string>
#include <string_view>
#include <vector>

namespace al::stracker {

// Stracker's loader.dll levels: DEBUG 0, INFO 1, WARN 2, ERR 3.
inline constexpr int kLevelError = 3;

// The loader-config.json keys Stracker's loader.dll reads, with its defaults.
struct Config {
    bool logfile = false;
    bool logcmd = false;
    int log_level = kLevelError;
    bool enable_plugin_loader = true;
};

// Reads loader-config.json like Stracker (nlohmann json, value(key, default))
// but never throws. Returns false if the text is not a JSON object; `out`
// then holds the defaults. Keys with an unexpected type or value keep their
// default and are listed in `problems`. Other keys (SPL's object, for one)
// are ignored.
bool parse_config(std::string_view text, Config& out, std::vector<std::string>& problems);

struct DirectoryEntry {
    std::wstring name;
    bool directory = false;
};

// The files Stracker loads from nativePC\plugins: regular files whose
// extension is exactly ".dll" (Stracker compares case-sensitively), sorted
// case-insensitively as NTFS lists them, so a name like "!First.dll"
// still comes first.
std::vector<std::wstring> plugin_files(std::vector<DirectoryEntry> entries);

}  // namespace al::stracker
