#include "core/plugins/discovery.hpp"

#include "common/log_file.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"
#include "core/plugins/manifest.hpp"

#include <windows.h>

#include <algorithm>

namespace al::plugins {
namespace {
bool ends_with_dll(std::wstring_view name) {
    return name.size() > 4 && iequals(name.substr(name.size() - 4), L".dll");
}
}  // namespace

Discovery discover(const std::wstring& root) {
    Discovery result;
    WIN32_FIND_DATAW entry{};
    const auto pattern = join_path(root, L"*");
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch,
                                         nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            result.skipped.push_back({root, "cannot list the plugins folder: " + to_utf8(win32_error_text(code))});
        }
        return result;
    }
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (ends_with_dll(name)) {
                result.skipped.push_back({name, "plugin DLLs go in their own folder: plugins\\<name>\\<name>.dll"});
            }
            continue;
        }
        if (!valid_name(std::wstring_view(name))) {
            result.skipped.push_back({name, "the folder name is not a valid plugin name "
                                            "(lower-case a-z, 0-9, '_', '-', '.')"});
            continue;
        }
        const auto folder = join_path(root, name);
        const auto dll = join_path(folder, name + L".dll");
        if (!regular_file(dll)) {
            result.skipped.push_back({name, "the folder has no " + to_utf8(name) + ".dll"});
            continue;
        }
        result.candidates.push_back({name, folder, dll});
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    std::sort(result.candidates.begin(), result.candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.name < b.name; });
    std::sort(result.skipped.begin(), result.skipped.end(),
              [](const Skipped& a, const Skipped& b) { return a.entry < b.entry; });
    return result;
}

}  // namespace al::plugins
