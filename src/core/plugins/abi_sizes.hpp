#pragma once

// The size each struct a plugin passes in had in the version that introduced
// it. AL accepts any size at least this, and reads a field added later only
// when the plugin's size covers it. Never compare with today's sizeof: it
// grows when a field is added, and plugins built before would be refused.
// (The plugin info struct has its own in manifest.cpp.)

#include "al/al_plugin.h"

#include <cstddef>

namespace al::plugins {

// al.hooks 1.0 (API 1.4)
inline constexpr std::size_t kHookSize10 = offsetof(al_hook_t, next) + sizeof(void*);
// al.input 1.0 (API 1.6)
inline constexpr std::size_t kHotkeySize10 = offsetof(al_hotkey_t, name) + sizeof(void*);
inline constexpr std::size_t kPadHotkeySize10 = offsetof(al_pad_hotkey_t, name) + sizeof(void*);

static_assert(kHookSize10 == 56 && kHotkeySize10 == 24 && kPadHotkeySize10 == 24,
              "a struct's 1.0 size is fixed for all of 1.x");

}  // namespace al::plugins
