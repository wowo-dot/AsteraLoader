#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

// MinHook adapter (buffer.c, AL_MINHOOK_NEAR_MEMORY). A trampoline block: one
// committed read-write-execute page of the reservation at an address in
// [lowest, highest), or NULL when it has none there; MinHook then searches as
// before.
extern "C" LPVOID WINAPI al_near_block(ULONG_PTR lowest, ULONG_PTR highest) noexcept;
// TRUE when block is a page of the reservation: MinHook's pages are
// decommitted and can be taken again, and MinHook must not release them.
// FALSE: not ours, MinHook releases it as before.
extern "C" BOOL WINAPI al_near_release(LPVOID block) noexcept;

namespace al::hooks {

// One reservation next to the game's image, made at bootstrap while the
// address space there is still free. The game later fills the space within
// reach with its own allocations; MinHook's trampoline blocks and al.patch's
// call stubs still find pages here. A page is committed when it is taken.
constexpr std::size_t kNearSize = 1024 * 1024;
constexpr std::size_t kNearPage = 0x1000;  // MinHook's MEMORY_BLOCK_SIZE

// Reserves kNearSize with every page within MinHook's reach (1 GB, less one
// allocation unit) of the whole image: below it if there is room, else above.
// Once per process. Returns the log line.
std::string reserve_near(const void* image, std::size_t image_size) noexcept;

// A committed read-write page at an address in [lowest, highest) for a call
// stub, or nullptr. The caller makes it executable; it is never given back.
void* near_stub_page(std::uintptr_t lowest, std::uintptr_t highest) noexcept;

struct NearCounts {
    std::uintptr_t base;  // 0: nothing reserved
    std::size_t pages, minhook, stubs;
};
NearCounts near_counts() noexcept;

// The counts, and the free address space MinHook could still take within
// reach of the image outside the reservation (64 KB units), as a log line.
std::string near_summary() noexcept;

}  // namespace al::hooks
