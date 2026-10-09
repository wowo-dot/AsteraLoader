#include "core/startup/entry_cache.hpp"

#include "common/paths.hpp"
#include "common/windows.hpp"

#include <charconv>
#include <cstdio>

namespace al::startup {
std::uint32_t cached_crt_rva(std::string_view text) noexcept {
    std::uint32_t result = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        if (end == std::string_view::npos) break;  // An unfinished append is not a record.
        auto line = text.substr(0, end);
        text.remove_prefix(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.starts_with(kCrtCachePrefix)) continue;
        line.remove_prefix(kCrtCachePrefix.size());
        std::uint32_t rva = 0;
        const auto parsed = std::from_chars(line.data(), line.data() + line.size(), rva, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == line.data() + line.size() && rva) result = rva;
    }
    return result;
}

std::wstring crt_cache_file(std::wstring_view logs_folder, std::wstring_view exe_hash) {
    if (logs_folder.empty() || exe_hash.size() != 64) return {};
    for (const auto c : exe_hash) if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return {};
    return join_path(join_path(directory_of(logs_folder), L"cache"), L"scan." + std::wstring(exe_hash) + L".txt");
}

std::uint32_t read_crt_cache(const std::wstring& path) {
    if (path.empty()) return 0;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > (4 << 20)) return 0;
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size()) return 0;
    return cached_crt_rva(text);
}

bool append_crt_cache(const std::wstring& path, std::uint32_t rva) {
    if (path.empty() || !rva) return false;
    CreateDirectoryW(std::wstring(directory_of(path)).c_str(), nullptr);
    Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;
    char number[16]{};
    std::snprintf(number, sizeof(number), "%x", static_cast<unsigned>(rva));
    // A leading delimiter also repairs an interrupted append at EOF without
    // replacing any plugin scanner records in this shared file.
    const auto line = "\n" + std::string(kCrtCachePrefix) + number + "\n";
    DWORD written = 0;
    return WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
}
}
