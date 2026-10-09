#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace al::startup {
// Bump when wrapper/common-main/cookie verification rules change.
inline constexpr std::string_view kCrtCachePrefix = "startup crt-entry-1 ";
std::uint32_t cached_crt_rva(std::string_view text) noexcept;
std::wstring crt_cache_file(std::wstring_view logs_folder, std::wstring_view exe_hash);
std::uint32_t read_crt_cache(const std::wstring& file);
bool append_crt_cache(const std::wstring& file, std::uint32_t rva);
}
