#pragma once

#include "core/startup/entry_signature.hpp"

namespace al::phases {
struct WinMainSite { std::uint32_t call; std::uint32_t target; };

// Search only the verified CRT common-main function. Pattern from SPL
// Core::WinMainCall (7be2393, MIT; existing notice retained). The image-base
// LEA and every branch target are also checked, and ambiguity is refused.
std::optional<WinMainSite> find_win_main(std::span<const std::uint8_t> image,
    std::span<const startup::CodeRange> code, startup::CodeRange common,
    std::string& reason);
bool calls_slot(const void* return_address, void** slot) noexcept;
}  // namespace al::phases
