#pragma once
#include "al/al_plugin.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace al::frame {
struct Site {
    void* target{};
    std::string pattern;
    std::int32_t offset{};
};
struct Sites {
    Site update, exiting;
    const void* const* main_object{};
    std::uint32_t exit_offset{};
    std::string problem;
};
inline constexpr char kFramePattern[] = "48 8B 8B 88 F8 0A 00 48 8B 01 FF 50 48";
inline constexpr char kExitPattern[] = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC 20 01 00 00";
// Supported addresses, never an unverified fallback. The registry checks the
// signature at each target, including its saved pre-patch bytes on a late join.
Sites game_sites(const al_game_t* identity, const void* image);
}  // namespace al::frame
