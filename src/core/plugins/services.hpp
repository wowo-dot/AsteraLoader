#pragma once

// CMFM's built-in services (API 1.1): "cmfm.game" and "cmfm.scanner".

#include "cmfm/cmfm_plugin.h"
#include "common/pattern.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cmfm::plugins {

class Scanner;

// "cmfm.game": filled once, on first use, and kept for the process.
class GameService {
public:
    // `image` is the game exe module; `game_folder` holds graphics_option.ini
    // (empty: the exe's folder). With a `scanner`, the version code is first
    // searched in code sections through its cache; without one, or if that
    // does not find exactly one match, the whole image is searched as
    // Stracker does.
    GameService(const void* image, std::wstring exe_sha256, std::wstring game_folder = {},
                Scanner* scanner = nullptr);
    const cmfm_game_t* table();

private:
    void fill();

    const void* image_;
    Scanner* scanner_;
    std::once_flag once_;
    std::string exe_sha256_;
    std::string version_;
    std::wstring exe_path_;
    std::wstring game_folder_;
    cmfm_game_t table_{};
};

// Reads "DirectX12Enable=On|Off" from graphics_option.ini text.
cmfm_graphics_t graphics_from_ini(std::string_view text);

// "cmfm.scanner": pattern search in the game exe, cached per exe hash in a
// text file. A cached result is used only after its matches are checked in
// memory; patterns without a match are not cached, so a function another
// hook has patched for a moment cannot poison the cache.
class Scanner {
public:
    // `cache_file` empty: no cache.
    Scanner(const void* image, std::wstring cache_file);

    struct Result {
        std::vector<const void*> matches;  // address order, at most CMFM_SCAN_MAX_MATCHES
        bool cached = false;
        double milliseconds = 0;
    };
    Result find(const Pattern& pattern, std::uint32_t where);

private:
    std::vector<std::pair<const std::uint8_t*, std::size_t>> ranges(std::uint32_t where) const;
    bool verify(const Pattern& pattern, const std::vector<std::uint32_t>& rvas) const;
    void load_cache();

    const std::uint8_t* base_;
    std::size_t size_ = 0;
    std::wstring cache_file_;
    std::mutex mutex_;
    bool cache_loaded_ = false;
    std::map<std::string, std::vector<std::uint32_t>> cache_;  // "<where> <pattern key>" -> RVAs
};

// instruction + length + the int32 at instruction + offset; null if unreadable.
void* rip_target(const void* instruction, std::uint32_t offset, std::uint32_t length) noexcept;

}  // namespace cmfm::plugins
