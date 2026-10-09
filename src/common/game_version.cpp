#include "common/game_version.hpp"

#include "common/memory.hpp"
#include "common/pattern.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace al {

bool plausible_version(std::string_view text) {
    if (text.empty() || text.size() > 63) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

GameVersion find_game_version(const void* image) {
    const auto* base = static_cast<const std::uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        !readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
        return {{}, "the game image header is unreadable"};
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::size_t size = nt->OptionalHeader.SizeOfImage;

    const auto pattern = parse_bits(kVersionPattern);
    std::vector<std::size_t> found;
    for (std::size_t offset = 0; offset < size && found.size() < 2;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(base + offset, &info, sizeof(info))) break;
        const auto region_end = std::min<std::size_t>(
            size, static_cast<std::size_t>(static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize - base));
        if (region_end <= offset) break;
        if (readable(base + offset, region_end - offset)) {
            find_all({base + offset, region_end - offset}, pattern, offset, found, 2);
        }
        offset = region_end;
    }
    if (found.size() != 1) return {{}, found.empty() ? "pattern not found" : "pattern found more than once"};
    return read_game_version_at(image, found[0]);
}

GameVersion read_game_version_at(const void* image, std::size_t match) {
    const auto* base = static_cast<const std::uint8_t*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE ||
        !readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) {
        return {{}, "the game image header is unreadable"};
    }
    const std::size_t size = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    if (match < kVersionInstructionBack || match >= size) return {{}, "the version instruction is outside the image"};

    const auto* instruction = base + match - kVersionInstructionBack;
    if (!readable(instruction, 7)) return {{}, "the version instruction is unreadable"};
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 3, sizeof(displacement));
    const auto address = version_pointer_address(reinterpret_cast<std::uintptr_t>(base + match), displacement);
    const auto* pointer = reinterpret_cast<const char* const*>(address);
    if (address < reinterpret_cast<std::uintptr_t>(base) ||
        address + sizeof(void*) > reinterpret_cast<std::uintptr_t>(base) + size || !readable(pointer, sizeof(*pointer))) {
        return {{}, "the version pointer is outside the game image"};
    }
    const char* text = *pointer;
    if (!text) return {{}, "the version pointer is not set yet"};
    std::string version;
    for (std::size_t k = 0; k < 64 && readable(text + k, 1) && text[k]; ++k) version += text[k];
    if (!plausible_version(version)) return {{}, "the version text is not readable text"};
    return {version, {}};
}

}  // namespace al
