#pragma once

#include <string>
#include <string_view>

namespace cmfm {

// Ordinal comparison that ignores case, as Windows compares file names.
bool iequals(std::wstring_view a, std::wstring_view b);

// The part after the last '\' or '/'; the whole path if there is none.
std::wstring_view file_name_of(std::wstring_view path);

// The part before the last '\' or '/'; empty if there is none.
std::wstring_view directory_of(std::wstring_view path);

std::wstring join_path(std::wstring_view directory, std::wstring_view name);

// Drive-absolute ("C:\x") or UNC ("\\server\x").
bool is_absolute(std::wstring_view path);

// "\\?\C:\x" -> "C:\x" and "\\?\UNC\server\x" -> "\\server\x", as returned by
// GetFinalPathNameByHandleW.
std::wstring strip_extended_prefix(std::wstring_view path);

// Replaces a leading user-profile directory with %USERPROFILE% for logs. Only
// whole path components match; an empty profile leaves the path unchanged.
std::wstring redact_profile(std::wstring_view path, std::wstring_view profile);

}  // namespace cmfm
