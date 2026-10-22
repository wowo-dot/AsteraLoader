// The reservation next to the game (core/hooks/near_memory) with MinHook and
// al.patch's writer: once the address space around a fake game is taken,
// a fake game without it fails at the 64th hook with MH_ERROR_MEMORY_ALLOC
// (as in a player's report, 2026-10-22), while one with it takes 300 hooks
// and a call stub. Pages MinHook gives back are taken again, never released
// with the reservation, and a moved RIP-relative address that no longer fits
// refuses the hook.
#include "core/hooks/near_memory.hpp"
#include "core/patch/writer.hpp"

#include <windows.h>
#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace hooks = al::hooks;

namespace {
int failures = 0;
void check(bool condition, const std::string& label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

constexpr std::size_t kImage = 0x10000;
constexpr std::size_t kFunctions = 300;
constexpr std::size_t kFunction = 16;
constexpr std::size_t kRipFar = 0x2000, kRipNear = 0x2010;  // mov rax,[rip+disp32]; ret
constexpr std::uintptr_t kFill = 0x80000000;  // more than MinHook's and al.patch's reach

int detour() { return 0; }

// sub rsp,28h; add rsp,28h; xor eax,eax; ret: MinHook moves the first eight bytes.
std::uint8_t* fake_game(std::uintptr_t preferred) {
    std::uint8_t* image = nullptr;
    for (auto at = preferred; !image && at < preferred + 0x80000000000ull; at += 0x10000000000ull)
        image = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), kImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) return nullptr;
    std::memset(image, 0xCC, kImage);
    const std::uint8_t body[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0xC4, 0x28, 0x31, 0xC0, 0xC3};
    for (std::size_t i = 0; i < kFunctions; ++i) std::memcpy(image + i * kFunction, body, sizeof(body));
    DWORD old = 0;
    VirtualProtect(image, kImage, PAGE_EXECUTE_READ, &old);
    return image;
}

void rip_function(std::uint8_t* at, std::int32_t displacement) {
    DWORD old = 0;
    VirtualProtect(at, 8, PAGE_EXECUTE_READWRITE, &old);
    const std::uint8_t mov[] = {0x48, 0x8B, 0x05};
    std::memcpy(at, mov, sizeof(mov));
    std::memcpy(at + 3, &displacement, sizeof(displacement));
    at[7] = 0xC3;
    VirtualProtect(at, 8, old, &old);
}

// Reserves every free allocation unit within kFill of the image, as the
// game's own allocations do during its start.
void take_space_around(const std::uint8_t* image) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t unit = info.dwAllocationGranularity;
    const auto middle = reinterpret_cast<std::uintptr_t>(image);
    auto at = (middle - kFill) & ~(unit - 1);
    while (at < middle + kFill) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory))) break;
        const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (memory.State == MEM_FREE) {
            const auto stop = (end < middle + kFill ? end : middle + kFill) & ~(unit - 1);
            if (stop > at) VirtualAlloc(reinterpret_cast<void*>(at), stop - at, MEM_RESERVE, PAGE_NOACCESS);
        }
        const auto next = (end + unit - 1) & ~(unit - 1);
        at = next > at ? next : at + unit;
    }
}

bool inside(const void* pointer, std::uintptr_t base) {
    const auto at = reinterpret_cast<std::uintptr_t>(pointer);
    return at >= base && at < base + hooks::kNearSize;
}
DWORD state(std::uintptr_t at) {
    MEMORY_BASIC_INFORMATION memory{};
    VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory));
    return memory.State;
}
DWORD protection(const void* at) {
    MEMORY_BASIC_INFORMATION memory{};
    VirtualQuery(at, &memory, sizeof(memory));
    return memory.Protect;
}
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
}  // namespace

int main() {
    check(MH_Initialize() == MH_OK, "MinHook initializes");
    auto* game = fake_game(0x500000000000ull);
    auto* other = fake_game(0x600000000000ull);
    if (!game || !other) {
        std::cerr << "FAIL: no fake game images\n";
        return 1;
    }

    check(!al_near_block(0, ~ULONG_PTR{0}) && !hooks::near_stub_page(0, ~std::uintptr_t{0}) && !hooks::near_counts().base,
          "nothing is handed out before the reservation");
    check(has(hooks::near_summary(), "reserved=none"), "the summary says when nothing is reserved");

    const auto reserved = hooks::reserve_near(game, kImage);
    const auto base = hooks::near_counts().base;
    check(has(reserved, "near_memory_reserved") && has(reserved, "side=below") && base &&
              base + hooks::kNearSize <= reinterpret_cast<std::uintptr_t>(game),
          "1 MB reserved right below the game: " + reserved);
    check(state(base) == MEM_RESERVE, "reserved, nothing committed yet");
    check(has(hooks::reserve_near(game, kImage), "already reserved"), "one reservation per process");

    // The other game has no reservation in reach: one MinHook block near it,
    // made while there is still room.
    void* original = nullptr;
    check(MH_CreateHook(other, reinterpret_cast<void*>(&detour), &original) == MH_OK, "the other game's first hook");

    take_space_around(game);
    take_space_around(other);
    check(has(hooks::near_summary(), "free_64kb_within_reach=0"), "no free space left near the game: " + hooks::near_summary());

    // Without the reservation: the block's 63 slots, then MH_ERROR_MEMORY_ALLOC.
    std::size_t made = 1;
    auto result = MH_OK;
    while (made < kFunctions && (result = MH_CreateHook(other + made * kFunction, reinterpret_cast<void*>(&detour), &original)) == MH_OK)
        ++made;
    check(made == 63 && result == MH_ERROR_MEMORY_ALLOC,
          "the other game: 63 hooks, then " + std::string(MH_StatusToString(result)) + " at hook " + std::to_string(made + 1));

    // With it: 300 hooks, every trampoline in the reservation.
    std::vector<void*> trampolines(kFunctions);
    bool all = true, within = true;
    for (std::size_t i = 0; i < kFunctions; ++i) {
        all = all && MH_CreateHook(game + i * kFunction, reinterpret_cast<void*>(&detour), &trampolines[i]) == MH_OK;
        within = within && inside(trampolines[i], base);
    }
    const auto counts = hooks::near_counts();
    check(all && within && counts.minhook == 5, "300 hooks in 5 pages of the reservation: " + hooks::near_summary());

    // A moved RIP-relative address that does not fit refuses the hook; one that fits does not.
    rip_function(game + kRipFar, 0x7FFFFFF0);
    rip_function(game + kRipNear, 0x100);
    check(MH_CreateHook(game + kRipFar, reinterpret_cast<void*>(&detour), &original) == MH_ERROR_UNSUPPORTED_FUNCTION,
          "an out-of-reach RIP-relative operand is refused");
    check(MH_CreateHook(game + kRipNear, reinterpret_cast<void*>(&detour), &original) == MH_OK && inside(original, base),
          "an in-reach RIP-relative operand is moved");

    // Pages MinHook gives back, the reservation's first among them, are
    // decommitted and taken again; the rest of the reservation stays.
    for (std::size_t i = 0; i < kFunctions; ++i) MH_RemoveHook(game + i * kFunction);
    MH_RemoveHook(game + kRipNear);
    check(hooks::near_counts().minhook == 0 && state(base) == MEM_RESERVE, "removed hooks give their pages back");
    check(MH_CreateHook(game, reinterpret_cast<void*>(&detour), &original) == MH_OK && inside(original, base) &&
              hooks::near_counts().minhook == 1,
          "a given-back page is taken again");

    // al.patch: a stub for the game from the reservation, none for the other one.
    al::patch::NativeWriter writer;
    void* stub = nullptr;
    const auto stub_problem = writer.stub(game + 0x100, reinterpret_cast<void*>(&detour), &stub);
    check(stub_problem.empty() && inside(stub, base) && protection(stub) == PAGE_EXECUTE_READ && hooks::near_counts().stubs == 1,
          "a call stub from the reservation, read-and-execute: " + stub_problem);
    const auto far_problem = writer.stub(other + 0x100, reinterpret_cast<void*>(&detour), &stub);
    check(has(far_problem, "no free memory within 2 GB") && has(far_problem, "near_memory base="),
          "no stub out of reach, with the reservation's state: " + far_problem);

    // Uninitializing gives MinHook's pages back and leaves the stub alone.
    check(MH_Uninitialize() == MH_OK, "MinHook uninitializes");
    const auto after = hooks::near_counts();
    check(after.minhook == 0 && after.stubs == 1 && state(base + hooks::kNearPage) == MEM_COMMIT,
          "MinHook's pages back, the stub kept");

    if (failures) {
        std::cerr << failures << " near-memory check(s) failed\n";
        return 1;
    }
    std::cout << "without the reservation the 64th hook fails (MH_ERROR_MEMORY_ALLOC); with it 300 hooks and a stub fit\n";
    return 0;
}
