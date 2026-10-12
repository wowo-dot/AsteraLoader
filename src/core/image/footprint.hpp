#pragma once

#include <cstddef>
#include <cstdint>

namespace al::image {

// The bytes a MinHook 1.3.4 hook on `target` takes: the entry instructions it
// copies into its trampoline (whole instructions, at least 5 bytes), which a
// later change would never reach; or, for a function shorter than 5 bytes,
// its hotpatch form, 5 bytes above the entry and the first ones at it.
// Decoded with MinHook's own disassembler; `room` bounds the read.
struct Footprint {
    std::uintptr_t begin = 0, end = 0;  // [begin, end)
};
Footprint hook_footprint(const std::uint8_t* target, std::size_t room) noexcept;

}  // namespace al::image
