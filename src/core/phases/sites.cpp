#include "core/phases/sites.hpp"
#include "common/memory.hpp"
#include <array>
#include <cstring>

namespace cmfm::phases {
namespace {
bool code_at(std::span<const std::uint8_t> image, std::span<const startup::CodeRange> code,
             std::int64_t rva, std::size_t count) {
    if (rva < 0) return false;
    const auto offset=static_cast<std::uint64_t>(rva);
    if (offset >= image.size() || count > image.size()-offset) return false;
    for (auto range : code) if (offset >= range.rva && offset-range.rva <= range.size &&
        count <= range.size-(offset-range.rva)) return true;
    return false;
}
std::int64_t relative(std::span<const std::uint8_t> image, std::size_t operand) {
    std::int32_t offset = 0;
    std::memcpy(&offset, image.data() + operand, 4);
    return static_cast<std::int64_t>(operand + 4) + offset;
}
}
std::optional<WinMainSite> find_win_main(std::span<const std::uint8_t> image,
    std::span<const startup::CodeRange> code, startup::CodeRange common, std::string& reason) {
    // E8 ???? 0F B7 D8 E8 ???? 4C 8B C0 44 8B CB 33 D2 48 8D 0D ???? E8 ????
    constexpr std::array<std::uint8_t, 33> pattern{0xe8,0,0,0,0,0x0f,0xb7,0xd8,0xe8,0,0,0,0,
        0x4c,0x8b,0xc0,0x44,0x8b,0xcb,0x33,0xd2,0x48,0x8d,0x0d,0,0,0,0,0xe8,0,0,0,0};
    if (common.size > 4096 || !code_at(image, code, common.rva, common.size) || common.size < pattern.size()) {
        reason = "CRT common-main extent unavailable"; return {};
    }
    std::optional<WinMainSite> found;
    for (std::size_t offset = 0; offset + pattern.size() <= common.size; ++offset) {
        const auto at = common.rva + offset;
        bool match = true;
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            if ((i >= 1 && i <= 4) || (i >= 9 && i <= 18) || (i >= 24 && i <= 27) || i >= 29) continue;
            if (image[at+i] != pattern[i]) { match = false; break; }
        }
        constexpr std::array<std::uint8_t,6> first{0x4c,0x8b,0xc0,0x44,0x8b,0xcb};
        constexpr std::array<std::uint8_t,6> second{0x44,0x8b,0xcb,0x4c,0x8b,0xc0};
        if (!match || (std::memcmp(image.data()+at+13,first.data(),6) && std::memcmp(image.data()+at+13,second.data(),6)) ||
            relative(image, at+24) != 0 || !code_at(image, code, relative(image, at+1), 1) ||
            !code_at(image, code, relative(image, at+9), 1) || !code_at(image, code, relative(image, at+29), 1)) continue;
        if (found) { reason = "multiple verified WinMain calls"; return {}; }
        found = WinMainSite{static_cast<std::uint32_t>(at+28), static_cast<std::uint32_t>(relative(image, at+29))};
    }
    reason = found ? "verified CRT WinMain call" : "no verified CRT WinMain call";
    return found;
}

bool calls_slot(const void* return_address, void** slot) noexcept {
    const auto end = reinterpret_cast<std::uintptr_t>(return_address);
    if (end < 6 || !readable(reinterpret_cast<const void*>(end-6), 6)) return false;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(end-6);
    std::int32_t offset = 0;
    if (bytes[0] == 0xff && bytes[1] == 0x15) {
        std::memcpy(&offset, bytes+2, 4);
        return end + static_cast<std::intptr_t>(offset) == reinterpret_cast<std::uintptr_t>(slot);
    }
    // MSVC also emits call rel32 to a leaf import thunk, which tail-jumps
    // through the same slot. It preserves the game's original return address.
    if (bytes[1] != 0xe8) return false;
    std::memcpy(&offset,bytes+2,4);
    const auto thunk=end+static_cast<std::intptr_t>(offset);
    if (!readable(reinterpret_cast<const void*>(thunk),6)) return false;
    const auto* jump=reinterpret_cast<const std::uint8_t*>(thunk);
    if (jump[0]!=0xff || jump[1]!=0x25) return false;
    std::memcpy(&offset,jump+2,4);
    return thunk+6+static_cast<std::intptr_t>(offset)==reinterpret_cast<std::uintptr_t>(slot);
}
}  // namespace cmfm::phases
