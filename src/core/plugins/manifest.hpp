#pragma once

#include "cmfm/cmfm_plugin.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cmfm::plugins {

// A plugin's declaration, copied out of its cmfm_plugin_info_t.
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
    cmfm_phase_fn on_phase = nullptr;
};

// The plugin-name rule from cmfm_plugin.h.
bool valid_name(std::string_view name);
bool valid_name(std::wstring_view name);

// Copies `info` into `out`, or sets `reason` to why CMFM cannot run the
// plugin. `folder_name` is the folder the DLL was found in. `exe_sha256` is
// the running exe's lower-case hex SHA-256, or empty if it is unknown.
bool read_manifest(const cmfm_plugin_info_t* info, std::wstring_view folder_name,
                   std::wstring_view exe_sha256, Manifest& out, std::string& reason);

}  // namespace cmfm::plugins
