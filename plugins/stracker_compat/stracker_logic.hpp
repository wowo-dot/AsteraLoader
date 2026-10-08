#pragma once

// Pure logic of the Stracker's Loader replacement, kept out of the DLLs so it
// can be unit-tested: loader-config.json, the game-version pattern, and the
// order Stracker loads nativePC\plugins in.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cmfm::stracker {

// Stracker's loader.dll levels: DEBUG 0, INFO 1, WARN 2, ERR 3.
inline constexpr int kLevelError = 3;

// The loader-config.json keys Stracker's loader.dll reads, with its defaults.
struct Config {
    bool logfile = false;
    bool logcmd = false;
    int log_level = kLevelError;
    bool enable_plugin_loader = true;
};

// Reads loader-config.json like Stracker (nlohmann json, value(key, default))
// but never throws. Returns false if the text is not a JSON object; `out`
// then holds the defaults. Keys with an unexpected type or value keep their
// default and are listed in `problems`. Other keys (SPL's object, for one)
// are ignored.
bool parse_config(std::string_view text, Config& out, std::vector<std::string>& problems);

// A byte pattern from Stracker's bit notation: '0' and '1' are bits, '.' is a
// wildcard bit, anything else is ignored.
struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;
};
Pattern parse_bits(std::string_view bits);

// Appends to `found` the offset of every (overlapping) match in `data`, up to
// `limit` matches in total.
void find_all(std::span<const std::uint8_t> data, const Pattern& pattern, std::size_t base,
              std::vector<std::size_t>& found, std::size_t limit);

// Stracker's FindVersion: the pattern, and where the version pointer is. The
// instruction that loads it starts 0x18 bytes before the match; it is 7 bytes
// long and holds a RIP-relative 32-bit displacement at offset 3.
inline constexpr char kVersionPattern[] =
    "01001... 10001101 01001100 ..100100 00110000 01001... 10001101 01000010 11110101";
inline constexpr std::size_t kVersionInstructionBack = 0x18;
inline std::uint64_t version_pointer_address(std::uint64_t match, std::int32_t displacement) {
    const auto instruction = match - kVersionInstructionBack;
    return instruction + 7 + static_cast<std::int64_t>(displacement);
}

// A version string CMFM accepts: 1 to 63 printable ASCII characters.
bool plausible_version(std::string_view text);

struct DirectoryEntry {
    std::wstring name;
    bool directory = false;
};

// The files Stracker loads from nativePC\plugins: regular files whose
// extension is exactly ".dll" (Stracker compares case-sensitively), sorted
// case-insensitively as NTFS lists them, so a name like "!CRCBypass.dll"
// still comes first.
std::vector<std::wstring> plugin_files(std::vector<DirectoryEntry> entries);

}  // namespace cmfm::stracker
