#include "core/plugins/manifest.hpp"

#include "common/log_file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace cmfm::plugins {
namespace {
// Upper bounds for data read out of a plugin, so a missing terminator in its
// static data cannot make CMFM read without end.
constexpr std::size_t kMaxText = 256;
constexpr std::size_t kMaxList = 64;
// API 1.0 plugins have the info struct up to on_phase; dependencies is 1.1.
constexpr std::size_t kInfoSize10 = offsetof(cmfm_plugin_info_t, dependencies);
constexpr std::size_t kInfoSize11 = offsetof(cmfm_plugin_info_t, dependencies) + sizeof(void*);

bool name_char(unsigned c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

template <typename Char>
bool valid_name_of(std::basic_string_view<Char> name) {
    if (name.empty() || name.size() > CMFM_NAME_MAX || name.front() == Char('.')) return false;
    for (const auto c : name) {
        if (!name_char(static_cast<unsigned>(static_cast<std::make_unsigned_t<Char>>(c)))) return false;
    }
    return true;
}

std::string text(const char* value, std::size_t limit = kMaxText) {
    return value ? std::string(value, strnlen(value, limit)) : std::string();
}

bool read_list(const char* const* list, std::vector<std::string>& out) {
    out.clear();
    if (!list) return true;
    for (std::size_t i = 0; i < kMaxList; ++i) {
        if (!list[i]) return true;
        out.push_back(text(list[i]));
    }
    return false;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool sha256_hex(std::string& value) {
    if (value.size() != 64) return false;
    for (auto& c : value) {
        c = lower(c);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string version_text(std::uint32_t version) {
    return std::to_string(CMFM_VERSION_MAJOR(version)) + "." + std::to_string(CMFM_VERSION_MINOR(version));
}
}  // namespace

std::optional<std::vector<std::uint32_t>> parse_version(std::string_view text) {
    std::size_t i = (!text.empty() && (text[0] == 'v' || text[0] == 'V')) ? 1 : 0;
    std::vector<std::uint32_t> parts;
    for (;;) {
        std::uint64_t number = 0;
        std::size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 10) {
            number = number * 10 + static_cast<std::uint64_t>(text[i++] - '0');
            ++digits;
        }
        if (!digits || number > 0xFFFFFFFFull) return std::nullopt;
        parts.push_back(static_cast<std::uint32_t>(number));
        if (i == text.size()) return parts;
        if (text[i] == '.' && parts.size() < 8) {
            ++i;
            continue;
        }
        if (text[i] == '-' || text[i] == '+' || text[i] == ' ') return parts;
        return std::nullopt;
    }
}

int compare_versions(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    for (std::size_t k = 0; k < std::max(a.size(), b.size()); ++k) {
        const auto x = k < a.size() ? a[k] : 0u, y = k < b.size() ? b[k] : 0u;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool valid_name(std::string_view name) { return valid_name_of(name); }
bool valid_name(std::wstring_view name) { return valid_name_of(name); }

bool read_manifest(const cmfm_plugin_info_t* info, std::wstring_view folder_name,
                   std::wstring_view exe_sha256, Manifest& out, std::string& reason) {
    out = {};
    if (!info) {
        reason = "cmfm_plugin_info returned nothing";
        return false;
    }
    if (info->size < kInfoSize10) {
        reason = "its plugin info is " + std::to_string(info->size) + " bytes, expected at least " +
                 std::to_string(kInfoSize10);
        return false;
    }
    const auto api = info->api_version;
    if (CMFM_VERSION_MAJOR(api) < CMFM_API_VERSION_MAJOR) {
        reason = "it was built for CMFM API " + version_text(api) + ", which this CMFM no longer supports";
        return false;
    }
    if (CMFM_VERSION_MAJOR(api) > CMFM_API_VERSION_MAJOR || CMFM_VERSION_MINOR(api) > CMFM_API_VERSION_MINOR) {
        reason = "it needs CMFM API " + version_text(api) + "; this CMFM provides " +
                 version_text(CMFM_API_VERSION) + ". Update CMFM.";
        return false;
    }

    out.name = text(info->name, CMFM_NAME_MAX + 1);
    if (!valid_name(out.name)) {
        reason = "its name is not a valid plugin name";
        return false;
    }
    bool same = out.name.size() == folder_name.size();
    for (std::size_t i = 0; same && i < out.name.size(); ++i) {
        same = folder_name[i] < 0x80 && lower(static_cast<char>(folder_name[i])) == out.name[i];
    }
    if (!same) {
        reason = "its name '" + out.name + "' does not match its folder '" + to_utf8(folder_name) + "'";
        return false;
    }
    out.display_name = info->display_name ? text(info->display_name) : out.name;
    out.version = text(info->version);
    out.api_version = api;

    out.phases = info->phases;
    out.on_phase = info->on_phase;
    if (out.phases & ~((1u << CMFM_PHASE_COUNT) - 1)) {
        reason = "it asks for phases this CMFM does not know";
        return false;
    }
    if (out.phases && !out.on_phase) {
        reason = "it asks for phases but has no on_phase function";
        return false;
    }
    out.order = info->order;
    if (!read_list(info->load_after, out.load_after) || !read_list(info->load_before, out.load_before) ||
        !read_list(info->exe_sha256, out.exe_sha256)) {
        reason = "one of its name lists has no NULL end within " + std::to_string(kMaxList) + " entries";
        return false;
    }

    // API 1.1: plugins this one requires. A 1.0 plugin's struct ends before them.
    if (info->size >= kInfoSize11 && info->dependencies) {
        std::size_t count = 0;
        for (const auto* entry = info->dependencies; entry->name; ++entry) {
            if (++count > kMaxList) {
                reason = "its dependency list has no end within " + std::to_string(kMaxList) + " entries";
                return false;
            }
            Dependency dependency{text(entry->name, CMFM_NAME_MAX + 1), text(entry->min_version)};
            if (!valid_name(dependency.name)) {
                reason = "it depends on a plugin with an invalid name";
                return false;
            }
            if (!dependency.min_version.empty() && !parse_version(dependency.min_version)) {
                reason = "its dependency on " + dependency.name + " has an unreadable version \"" +
                         dependency.min_version + "\"";
                return false;
            }
            out.dependencies.push_back(std::move(dependency));
        }
    }

    if (!out.exe_sha256.empty()) {
        for (auto& entry : out.exe_sha256) {
            if (!sha256_hex(entry)) {
                reason = "its exe_sha256 list has an entry that is not a SHA-256";
                return false;
            }
        }
        if (exe_sha256.empty()) {
            reason = "it supports only specific game exes, and this exe's SHA-256 is not available";
            return false;
        }
        std::string exe;
        for (const auto c : exe_sha256) exe += lower(static_cast<char>(c));
        bool listed = false;
        for (const auto& entry : out.exe_sha256) listed = listed || entry == exe;
        if (!listed) {
            reason = "it does not support this game exe (SHA-256 " + exe.substr(0, 12) + "...)";
            return false;
        }
    }
    return true;
}

}  // namespace cmfm::plugins
