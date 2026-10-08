#pragma once

#include <string>
#include <vector>

namespace cmfm::plugins {

// A plugin folder that holds its DLL: <root>\<name>\<name>.dll.
struct Candidate {
    std::wstring name;
    std::wstring folder;  // <root>\<name>, as listed (the virtual path under MO2)
    std::wstring dll;
};

// Something in the plugins folder that is not a usable plugin, and why.
struct Skipped {
    std::wstring entry;  // the folder or file name inside <root>
    std::string reason;
};

struct Discovery {
    std::vector<Candidate> candidates;  // sorted by name
    std::vector<Skipped> skipped;
};

// Lists <root>. Only <root>\<name>\<name>.dll is a plugin; other files in a
// plugin's folder belong to the plugin. A missing root means no plugins.
Discovery discover(const std::wstring& root);

}  // namespace cmfm::plugins
