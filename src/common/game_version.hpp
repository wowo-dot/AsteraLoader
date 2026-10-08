#pragma once

// MHW's build string ("421810"), found the way Stracker's Loader finds it:
// a code pattern whose instruction 0x18 bytes earlier loads a pointer to the
// string. Used by CMFM's game-identity service and by the Stracker shim.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cmfm {

inline constexpr char kVersionPattern[] =
    "01001... 10001101 01001100 ..100100 00110000 01001... 10001101 01000010 11110101";
inline constexpr std::size_t kVersionInstructionBack = 0x18;

// The instruction that loads the version pointer starts 0x18 bytes before
// the match; it is 7 bytes long with a RIP-relative 32-bit displacement at
// offset 3.
inline std::uint64_t version_pointer_address(std::uint64_t match, std::int32_t displacement) {
    const auto instruction = match - kVersionInstructionBack;
    return instruction + 7 + static_cast<std::int64_t>(displacement);
}

// 1 to 63 printable ASCII characters.
bool plausible_version(std::string_view text);

struct GameVersion {
    std::string text;     // empty if not found
    std::string failure;  // why, if not found
};

// Searches every readable region of the loaded image, needs exactly one match
// (as Stracker does), and only reads memory that is readable. Call after the
// game's code is unpacked (PRE_MAIN or later).
GameVersion find_game_version(const void* image);

}  // namespace cmfm
