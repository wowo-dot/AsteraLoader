#include "core/plugins/manifest.hpp"

#include "common/log_file.hpp"

#include <cstring>
#include <type_traits>

namespace cmfm::plugins {
namespace {
// Upper bounds for data read out of a plugin, so a missing terminator in its
// static data cannot make CMFM read without end.
constexpr std::size_t kMaxText = 256;
constexpr std::size_t kMaxList = 64;

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

bool valid_name(std::string_view name) { return valid_name_of(name); }
bool valid_name(std::wstring_view name) { return valid_name_of(name); }

bool read_manifest(const cmfm_plugin_info_t* info, std::wstring_view folder_name,
                   std::wstring_view exe_sha256, Manifest& out, std::string& reason) {
    out = {};
    if (!info) {
        reason = "cmfm_plugin_info returned nothing";
        return false;
    }
    if (info->size < sizeof(cmfm_plugin_info_t)) {
        reason = "its plugin info is " + std::to_string(info->size) + " bytes, expected at least " +
                 std::to_string(sizeof(cmfm_plugin_info_t));
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
