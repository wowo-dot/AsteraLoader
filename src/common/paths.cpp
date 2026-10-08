#include "common/paths.hpp"

#include <windows.h>

#include <algorithm>

namespace cmfm {

namespace {

bool is_separator(wchar_t c) {
    return c == L'\\' || c == L'/';
}

std::size_t last_separator(std::wstring_view path) {
    return path.find_last_of(L"\\/");
}

}  // namespace

bool iequals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring_view file_name_of(std::wstring_view path) {
    const auto separator = last_separator(path);
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

std::wstring_view directory_of(std::wstring_view path) {
    const auto separator = last_separator(path);
    const bool extended_drive_root = separator == 6 && path.size() >= 7 &&
        path.substr(0, 4) == L"\\\\?\\" && path[5] == L':';
    if (separator == 0 || (separator == 2 && path.size() >= 3 && path[1] == L':') || extended_drive_root) {
        return path.substr(0, separator + 1);
    }
    return separator == std::wstring_view::npos ? std::wstring_view{} : path.substr(0, separator);
}

std::wstring join_path(std::wstring_view directory, std::wstring_view name) {
    std::wstring joined(directory);
    if (!joined.empty() && !is_separator(joined.back())) {
        joined.push_back(L'\\');
    }
    joined.append(name);
    return joined;
}

bool is_absolute(std::wstring_view path) {
    if (path.size() >= 3 && path[1] == L':' && is_separator(path[2])) {
        return true;
    }
    return path.size() >= 2 && is_separator(path[0]) && is_separator(path[1]);
}

std::wstring strip_extended_prefix(std::wstring_view path) {
    constexpr std::wstring_view unc_prefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view prefix = L"\\\\?\\";
    if (path.size() >= unc_prefix.size() && iequals(path.substr(0, unc_prefix.size()), unc_prefix)) {
        return L"\\\\" + std::wstring(path.substr(unc_prefix.size()));
    }
    if (path.substr(0, prefix.size()) == prefix) {
        return std::wstring(path.substr(prefix.size()));
    }
    return std::wstring(path);
}

std::wstring redact_profile(std::wstring_view path, std::wstring_view profile) {
    const auto plain_path = strip_extended_prefix(path);
    const auto plain_profile = strip_extended_prefix(profile);
    profile = plain_profile;
    while (!profile.empty() && is_separator(profile.back())) {
        profile.remove_suffix(1);
    }
    std::wstring prefix = plain_path.substr(0, profile.size());
    std::wstring comparison_profile(profile);
    std::replace(prefix.begin(), prefix.end(), L'/', L'\\');
    std::replace(comparison_profile.begin(), comparison_profile.end(), L'/', L'\\');
    if (profile.empty() || plain_path.size() < profile.size() ||
        !iequals(prefix, comparison_profile)) {
        return std::wstring(path);
    }
    if (plain_path.size() > profile.size() && !is_separator(plain_path[profile.size()])) {
        return std::wstring(path);
    }
    return L"%USERPROFILE%" + plain_path.substr(profile.size());
}

}  // namespace cmfm
