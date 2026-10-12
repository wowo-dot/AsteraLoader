#include "core/image/footprint.hpp"

#include "hde/hde64.h"

#include <algorithm>

namespace al::image {

Footprint hook_footprint(const std::uint8_t* target, std::size_t room) noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(target);
    constexpr std::size_t kJump = 5;  // MinHook's entry jump
    std::size_t copied = 0;
    while (copied < kJump && copied < room) {
        hde64s instruction{};
        const auto length = hde64_disasm(target + copied, &instruction);
        if ((instruction.flags & F_ERROR) || !length || copied + length > room) break;
        copied += length;
        // The function ends here (ret, or a jump it does not come back from):
        // MinHook copies no further.
        const auto op = instruction.opcode;
        if (op == 0xC3 || op == 0xC2 || op == 0xE9 || op == 0xEB || (op == 0xFF && instruction.modrm_reg == 4)) break;
    }
    if (copied >= kJump) return {at, at + copied};
    // Too short for the entry jump: MinHook's hotpatch form, a jump in the 5
    // bytes above and a short jump at the entry.
    return {at - kJump, at + (std::max)(copied, std::size_t{2})};
}

}  // namespace al::image
