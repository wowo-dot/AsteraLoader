#pragma once
#include "core/frame/sites.hpp"

namespace al::input {
// The game's own input functions (design-input-1.6.md, "Found by static
// analysis"). Hooked through al.hooks, so the Steam overlay or anything else
// wrapping DirectInput makes no difference.
struct Sites {
    frame::Site keyboard;  // keyboard update: fills and edge-detects the key maps
    frame::Site mouse;     // mouse read: fills the mouse state from DirectInput
    frame::Site pad;       // XInput pad processing: copies one XINPUT_STATE into a pad slot
    std::string problem;
};
inline constexpr char kKeyboardPattern[] =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 33 FF 48 8D B1 38 01 00 00";
inline constexpr char kMousePattern[] = "40 55 53 56 57 41 56 48 8B EC 48 81 EC 80 00 00 00";
inline constexpr char kPadPattern[] = "48 89 5C 24 08 57 44 8B 9A 60 01 00 00";

// Game layouts the detours rely on (MHW 421810).
inline constexpr std::size_t kKeyMaps = 0x138;      // keyboard: held, previous, pressed, released, changed, repeat; 8 dwords each
inline constexpr std::size_t kMouseMove = 20;       // mouse state: x, y and wheel deltas, 3 ints
inline constexpr std::size_t kMouseButtons = 32;    // mouse state: buttons as bits 0..7
inline constexpr std::size_t kPadUser = 0x14;       // pad slot: XInput user index
inline constexpr std::size_t kPadTriggerLevel = 25; // pad slot: the game's trigger threshold, one byte

// Supported addresses, never an unverified fallback.
Sites game_sites(const al_game_t* identity, const void* image);
}  // namespace al::input
