#pragma once

#include "al/al_plugin.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace al::plugins {

// A plugin this one requires (API 1.1).
struct Dependency {
    std::string name;
    std::string min_version;  // empty: any version
};

// A plugin's declaration, copied out of its al_plugin_info_t.
struct Manifest {
    std::string name;
    std::string display_name;
    std::string version;
    std::uint32_t api_version = 0;
    std::uint32_t phases = 0;
    std::int32_t order = 0;
    std::vector<std::string> load_after;
    std::vector<std::string> load_before;
    std::vector<std::string> exe_sha256;  // empty: any exe
    al_phase_fn on_phase = nullptr;
    std::vector<Dependency> dependencies;  // API 1.1
};

// Dotted decimal numbers ("0.14", "v1.2.3"); a suffix after '-', '+' or a
// space ("0.14.0-beta") is ignored. Empty optional if there is no number.
std::optional<std::vector<std::uint32_t>> parse_version(std::string_view text);

// Negative, zero or positive as a is older than, equal to or newer than b;
// missing parts count as 0 ("1.2" equals "1.2.0").
int compare_versions(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b);

// The plugin-name rule from al_plugin.h.
bool valid_name(std::string_view name);
bool valid_name(std::wstring_view name);

// Copies `info` into `out`, or sets `reason` to why AL cannot run the
// plugin. `folder_name` is the folder the DLL was found in. `exe_sha256` is
// the running exe's lower-case hex SHA-256, or empty if it is unknown.
bool read_manifest(const al_plugin_info_t* info, std::wstring_view folder_name,
                   std::wstring_view exe_sha256, Manifest& out, std::string& reason);

}  // namespace al::plugins
