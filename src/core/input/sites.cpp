#include "core/input/sites.hpp"
#include "common/memory.hpp"
#include <windows.h>
#include <cstring>

namespace al::input {
Sites game_sites(const al_game_t* identity, const void* image) {
    Sites sites;
    constexpr char hash[] = "c2ebbbd2c49f216d484e31a5219bed419eb1e5e7d206d02cba040a3ab79d90ea";
    if (!identity || !identity->game_version || std::strcmp(identity->game_version, "421810") ||
        !identity->exe_sha256 || std::strcmp(identity->exe_sha256, hash)) {
        sites.problem = "input sites require the verified MHW 421810 executable (version and SHA-256)";
        return sites;
    }
    const auto base = static_cast<const std::uint8_t*>(image);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!readable(base, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x100000) {
        sites.problem = "input sites: unreadable game image"; return sites;
    }
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->OptionalHeader.SizeOfImage < 0x51c5328) {
        sites.problem = "input sites: invalid or truncated game image"; return sites;
    }
    sites.keyboard = {const_cast<std::uint8_t*>(base + 0x22e74f0), kKeyboardPattern, 0};
    sites.mouse = {const_cast<std::uint8_t*>(base + 0x22e9270), kMousePattern, 0};
    sites.pad = {const_cast<std::uint8_t*>(base + 0x22a1280), kPadPattern, 0};
    return sites;
}
}  // namespace al::input
