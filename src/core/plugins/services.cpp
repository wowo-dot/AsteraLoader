#include "core/plugins/services.hpp"

#include "common/game_version.hpp"
#include "common/log_file.hpp"
#include "common/memory.hpp"
#include "common/paths.hpp"
#include "common/windows.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace al::plugins {
namespace {
bool read_small_file(const std::wstring& path, std::string& out, std::size_t limit) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart > static_cast<LONGLONG>(limit)) return false;
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    return out.empty() ||
           (ReadFile(file.get(), out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
}

const IMAGE_NT_HEADERS64* nt_headers(const void* image) {
    const auto* base = static_cast<const std::uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    return nt;
}

std::string trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return std::string(text);
}

bool same_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        const auto x = a[k] >= 'A' && a[k] <= 'Z' ? a[k] - 'A' + 'a' : a[k];
        const auto y = b[k] >= 'A' && b[k] <= 'Z' ? b[k] - 'A' + 'a' : b[k];
        if (x != y) return false;
    }
    return true;
}

double milliseconds_since(const LARGE_INTEGER& start) {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart ? static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
                                    static_cast<double>(frequency.QuadPart)
                              : 0.0;
}
}  // namespace

al_graphics_t graphics_from_ini(std::string_view text) {
    al_graphics_t graphics = AL_GRAPHICS_UNKNOWN;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos || !same_ascii(trim(line.substr(0, equals)), "DirectX12Enable")) continue;
        const auto value = trim(line.substr(equals + 1));
        graphics = same_ascii(value, "On") ? AL_GRAPHICS_DX12 : same_ascii(value, "Off") ? AL_GRAPHICS_DX11
                                                                                             : AL_GRAPHICS_UNKNOWN;
    }
    return graphics;
}

GameService::GameService(const void* image, std::wstring exe_sha256, std::wstring game_folder, Scanner* scanner)
    : image_(image), scanner_(scanner), exe_sha256_(to_utf8(exe_sha256)), game_folder_(std::move(game_folder)) {}

const al_game_t* GameService::table() {
    std::call_once(once_, [this] { fill(); });
    return &table_;
}

void GameService::fill() {
    exe_path_ = module_path();
    if (game_folder_.empty()) game_folder_ = std::wstring(directory_of(exe_path_));
    // The version code is code: a code-section search is quick (1.8 ms in the
    // game, against about 12 ms for the whole image) and cached per exe hash.
    if (scanner_) {
        const auto found = scanner_->find(parse_bits(kVersionPattern), AL_SCAN_CODE);
        if (found.matches.size() == 1) {
            const auto offset = static_cast<std::size_t>(static_cast<const std::uint8_t*>(found.matches[0]) -
                                                         static_cast<const std::uint8_t*>(image_));
            version_ = read_game_version_at(image_, offset).text;
        }
    }
    if (version_.empty()) version_ = find_game_version(image_).text;
    const auto* nt = nt_headers(image_);
    std::string ini;
    const auto graphics = read_small_file(join_path(game_folder_, L"graphics_option.ini"), ini, 1 << 16)
                              ? graphics_from_ini(ini)
                              : AL_GRAPHICS_UNKNOWN;
    table_ = {sizeof(al_game_t),
              AL_GAME_VERSION,
              exe_sha256_.empty() ? nullptr : exe_sha256_.c_str(),
              version_.empty() ? nullptr : version_.c_str(),
              exe_path_.c_str(),
              game_folder_.c_str(),
              nt ? nt->FileHeader.TimeDateStamp : 0u,
              graphics};
}

Scanner::Scanner(const void* image, std::wstring cache_file)
    : base_(static_cast<const std::uint8_t*>(image)), cache_file_(std::move(cache_file)) {
    if (const auto* nt = nt_headers(image)) size_ = nt->OptionalHeader.SizeOfImage;
}

std::vector<std::pair<const std::uint8_t*, std::size_t>> Scanner::ranges(std::uint32_t where) const {
    std::vector<std::pair<const std::uint8_t*, std::size_t>> out;
    const auto* nt = nt_headers(base_);
    if (!nt) return out;
    if (where == AL_SCAN_IMAGE) {
        out.emplace_back(base_, size_);
        return out;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    if (!readable(section, sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections)) return out;
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const std::size_t start = section->VirtualAddress;
        const std::size_t length = std::min<std::size_t>(section->Misc.VirtualSize, size_ - std::min(size_, start));
        if (length) out.emplace_back(base_ + start, length);
    }
    return out;
}

bool Scanner::verify(const Pattern& pattern, const std::vector<std::uint32_t>& rvas) const {
    const auto size = pattern.bytes.size();
    for (const auto rva : rvas) {
        if (rva + size > size_ || !readable(base_ + rva, size)) return false;
        for (std::size_t k = 0; k < size; ++k) {
            if ((base_[rva + k] & pattern.mask[k]) != (pattern.bytes[k] & pattern.mask[k])) return false;
        }
    }
    return true;
}

void Scanner::load_cache() {
    cache_loaded_ = true;
    std::string text;
    if (cache_file_.empty() || !read_small_file(cache_file_, text, 4 << 20)) return;
    // Lines: "<where> <pattern key> <rva>,<rva>,...", the last one per key wins.
    std::size_t at = 0;
    while (at < text.size()) {
        const auto end = std::min(text.find('\n', at), text.size());
        const auto line = std::string_view(text).substr(at, end - at);
        at = end + 1;
        const auto first = line.find(' '), second = line.find(' ', first + 1);
        if (first == std::string_view::npos || second == std::string_view::npos) continue;
        std::vector<std::uint32_t> rvas;
        bool ok = true;
        auto list = line.substr(second + 1);
        while (ok && !list.empty() && list.back() == '\r') list.remove_suffix(1);
        while (ok && !list.empty()) {
            const auto comma = list.find(',');
            const auto item = std::string(list.substr(0, comma));
            char* tail = nullptr;
            const auto value = std::strtoul(item.c_str(), &tail, 16);
            ok = !item.empty() && tail && !*tail && value < size_;
            if (ok) rvas.push_back(static_cast<std::uint32_t>(value));
            list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
        }
        if (ok && !rvas.empty() && rvas.size() <= AL_SCAN_MAX_MATCHES) cache_[std::string(line.substr(0, second))] = rvas;
    }
}

Scanner::Result Scanner::find(const Pattern& pattern, std::uint32_t where) {
    LARGE_INTEGER start{};
    QueryPerformanceCounter(&start);
    Result result;
    const auto key = std::to_string(where) + " " + pattern_key(pattern);
    {
        std::lock_guard lock(mutex_);
        if (!cache_loaded_) load_cache();
        const auto hit = cache_.find(key);
        if (hit != cache_.end() && verify(pattern, hit->second)) {
            for (const auto rva : hit->second) result.matches.push_back(base_ + rva);
            result.cached = true;
            result.milliseconds = milliseconds_since(start);
            return result;
        }
    }

    std::vector<std::size_t> found;
    for (const auto& [begin, length] : ranges(where)) {
        // Only readable regions, as VirtualQuery splits them.
        for (std::size_t offset = 0; offset < length && found.size() < AL_SCAN_MAX_MATCHES;) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(begin + offset, &info, sizeof(info))) break;
            const auto region_end = std::min<std::size_t>(
                length, static_cast<std::size_t>(static_cast<const std::uint8_t*>(info.BaseAddress) +
                                                 info.RegionSize - begin));
            if (region_end <= offset) break;
            if (readable(begin + offset, region_end - offset)) {
                find_all({begin + offset, region_end - offset}, pattern,
                         static_cast<std::size_t>(begin - base_) + offset, found, AL_SCAN_MAX_MATCHES);
            }
            offset = region_end;
        }
    }
    std::sort(found.begin(), found.end());
    for (const auto rva : found) result.matches.push_back(base_ + rva);
    result.milliseconds = milliseconds_since(start);
    if (found.empty()) return result;

    std::lock_guard lock(mutex_);
    std::vector<std::uint32_t> rvas;
    for (const auto rva : found) rvas.push_back(static_cast<std::uint32_t>(rva));  // within SizeOfImage
    cache_[key] = rvas;
    if (cache_file_.empty()) return result;
    std::string line = key + " ";
    for (std::size_t k = 0; k < rvas.size(); ++k) {
        char hex[16]{};
        std::snprintf(hex, sizeof(hex), "%s%x", k ? "," : "", rvas[k]);
        line += hex;
    }
    line += "\n";
    const std::wstring folder(directory_of(cache_file_));
    CreateDirectoryW(folder.c_str(), nullptr);
    Handle file(CreateFileW(cache_file_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    if (file) WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    return result;
}

void* rip_target(const void* instruction, std::uint32_t offset, std::uint32_t length) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(instruction);
    if (!instruction || !readable(bytes + offset, sizeof(std::int32_t))) return nullptr;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes + offset, sizeof(displacement));
    return const_cast<std::uint8_t*>(bytes + length + displacement);
}

}  // namespace al::plugins
