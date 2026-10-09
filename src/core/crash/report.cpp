#include "core/crash/report.hpp"

namespace al::crash {
namespace {
wchar_t lower(wchar_t c) noexcept {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// Where `needle` (lower case) starts in `text`, ignoring case; npos if absent.
std::size_t find_folded(std::wstring_view text, std::wstring_view needle) noexcept {
    if (needle.size() > text.size()) return std::wstring_view::npos;
    for (std::size_t at = 0; at + needle.size() <= text.size(); ++at) {
        std::size_t k = 0;
        while (k < needle.size() && lower(text[at + k]) == needle[k]) ++k;
        if (k == needle.size()) return at;
    }
    return std::wstring_view::npos;
}

std::wstring_view file_name(std::wstring_view path) noexcept {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool same_folded(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        if (lower(a[k]) != lower(b[k])) return false;
    }
    return true;
}
}  // namespace

Owner classify(std::wstring_view path, bool is_core, bool is_game) noexcept {
    const auto name = file_name(path);
    if (is_core) return {OwnerKind::core, name};
    if (is_game) return {OwnerKind::game, name};
    constexpr std::wstring_view plugins = L"\\astera\\plugins\\";
    if (const auto at = find_folded(path, plugins); at != std::wstring_view::npos) {
        auto rest = path.substr(at + plugins.size());
        const auto end = rest.find_first_of(L"\\/");
        if (end != std::wstring_view::npos && end > 0) return {OwnerKind::plugin, rest.substr(0, end)};
    }
    if (find_folded(path, L"\\nativepc\\plugins\\") != std::wstring_view::npos) return {OwnerKind::stracker_plugin, name};
    for (const auto folder : {L"\\windows\\system32\\", L"\\windows\\syswow64\\", L"\\windows\\winsxs\\"}) {
        if (find_folded(path, folder) != std::wstring_view::npos) return {OwnerKind::windows, name};
    }
    if (path.empty()) return {OwnerKind::unknown, name};
    return {OwnerKind::other, name};
}

const char* owner_label(OwnerKind kind) noexcept {
    switch (kind) {
    case OwnerKind::game: return "the game";
    case OwnerKind::core: return "Astera Loader";
    case OwnerKind::plugin: return "plugin";
    case OwnerKind::stracker_plugin: return "Stracker plugin";
    case OwnerKind::windows: return "Windows";
    case OwnerKind::other: return "module";
    default: return "unknown code";
    }
}

bool removable(OwnerKind kind) noexcept {
    return kind == OwnerKind::plugin || kind == OwnerKind::stracker_plugin;
}

bool fatal(unsigned long code) noexcept {
    switch (code) {
    case 0xC0000005u:  // access violation
    case 0xC0000006u:  // in-page error
    case 0xC000001Du:  // illegal instruction
    case 0xC0000096u:  // privileged instruction
    case 0xC0000094u:  // integer divide by zero
    case 0xC0000095u:  // integer overflow
    case 0xC000008Cu:  // array bounds exceeded
    case 0x80000002u:  // datatype misalignment
    case 0xC00000FDu:  // stack overflow
    case 0xC0000374u:  // heap corruption
    case 0xC0000420u:  // assertion failure
        return true;
    default:
        return false;
    }
}

const char* exception_name(unsigned long code) noexcept {
    switch (code) {
    case 0xC0000005u: return "access violation";
    case 0xC0000006u: return "in-page error";
    case 0xC000001Du: return "illegal instruction";
    case 0xC0000096u: return "privileged instruction";
    case 0xC0000094u: return "integer divide by zero";
    case 0xC0000095u: return "integer overflow";
    case 0xC000008Cu: return "array bounds exceeded";
    case 0x80000002u: return "datatype misalignment";
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000374u: return "heap corruption";
    case 0xC0000420u: return "assertion failure";
    case 0xE06D7363u: return "C++ exception";
    default: return "exception";
    }
}

Minidump minidump_setting(std::string_view text) noexcept {
    auto setting = Minidump::off;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (const auto comment = line.find_first_of(";#"); comment != std::string_view::npos) line = line.substr(0, comment);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos || !same_folded(trim(line.substr(0, equals)), "crash_minidump")) continue;
        const auto value = trim(line.substr(equals + 1));
        setting = same_folded(value, "small") ? Minidump::small : same_folded(value, "full") ? Minidump::full
                                                                                              : Minidump::off;
    }
    return setting;
}

}  // namespace al::crash
