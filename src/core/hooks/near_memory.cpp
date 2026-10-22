#include "core/hooks/near_memory.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
using al::hooks::kNearPage;
using al::hooks::kNearSize;

constexpr std::uintptr_t kReach = 0x40000000 - 0x10000;  // MinHook's MAX_MEMORY_RANGE, less one allocation unit
constexpr std::size_t kPages = kNearSize / kNearPage;
enum class Use : std::uint8_t { free, minhook, stub };

struct Near {
    SRWLOCK lock = SRWLOCK_INIT;
    bool tried = false;
    std::uintptr_t image_begin = 0, image_end = 0, base = 0;
    std::array<Use, kPages> use{};
} g_near;

class Exclusive {
public:
    Exclusive() noexcept { AcquireSRWLockExclusive(&g_near.lock); }
    ~Exclusive() { ReleaseSRWLockExclusive(&g_near.lock); }
    Exclusive(const Exclusive&) = delete;
    Exclusive& operator=(const Exclusive&) = delete;
};

std::uintptr_t align_down(std::uintptr_t value, std::uintptr_t unit) noexcept { return value & ~(unit - 1); }
std::uintptr_t align_up(std::uintptr_t value, std::uintptr_t unit) noexcept { return (value + unit - 1) & ~(unit - 1); }

// The lowest and highest base of size bytes whose every page is within reach
// of [begin, end).
struct Window { std::uintptr_t floor, ceiling; };
Window window(std::uintptr_t begin, std::uintptr_t end, std::size_t size) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto lowest = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto top = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress) + 1;
    const auto floor = end > lowest + kReach ? end - kReach : lowest;
    const auto ceiling = (top - begin > kReach ? begin + kReach : top) - size;
    return {floor, ceiling};
}

void* reserve_at(std::uintptr_t at) noexcept {
    return VirtualAlloc(reinterpret_cast<void*>(at), kNearSize, MEM_RESERVE, PAGE_NOACCESS);
}

// Downwards from the image, as MinHook looks.
void* reserve_below(std::uintptr_t begin, Window range, std::uintptr_t unit) noexcept {
    if (begin < range.floor + kNearSize) return nullptr;
    auto at = align_down(begin - kNearSize, unit);
    while (at >= range.floor) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory))) break;
        if (memory.State == MEM_FREE && memory.RegionSize >= kNearSize)
            if (auto* reserved = reserve_at(at)) return reserved;
        // Below what occupies [at, at + kNearSize): the free run's end or the allocation.
        const auto blocker = memory.State == MEM_FREE
            ? reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize
            : reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        if (blocker < kNearSize + unit) break;
        const auto next = align_down(blocker - kNearSize, unit);
        if (next < at) at = next;
        else if (at >= unit) at -= unit;
        else break;
    }
    return nullptr;
}

void* reserve_above(std::uintptr_t end, Window range, std::uintptr_t unit) noexcept {
    auto at = align_up(end, unit);
    while (at <= range.ceiling) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory))) break;
        if (memory.State == MEM_FREE && memory.RegionSize >= kNearSize)
            if (auto* reserved = reserve_at(at)) return reserved;
        const auto next = align_up(reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize, unit);
        at = next > at ? next : at + unit;
    }
    return nullptr;
}

// Allocation units MinHook could still take for any target in the image.
std::size_t free_units(std::uintptr_t begin, std::uintptr_t end) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t unit = info.dwAllocationGranularity;
    const auto range = window(begin, end, unit);
    const auto last = align_down(range.ceiling, unit) + unit;
    std::size_t count = 0;
    auto at = align_up(range.floor, unit);
    while (at < last) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &memory, sizeof(memory))) break;
        const auto region_end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        if (memory.State == MEM_FREE) {
            const auto stop = std::min(align_down(region_end, unit), last);
            if (stop > at) count += (stop - at) / unit;
        }
        const auto next = align_up(region_end, unit);
        at = next > at ? next : at + unit;
    }
    return count;
}

void* take(std::uintptr_t lowest, std::uintptr_t highest, Use use, DWORD protect) noexcept {
    Exclusive hold;
    if (!g_near.base) return nullptr;
    for (std::size_t i = 0; i < kPages; ++i) {
        const auto page = g_near.base + i * kNearPage;
        if (g_near.use[i] != Use::free || page < lowest || page >= highest) continue;
        if (!VirtualAlloc(reinterpret_cast<void*>(page), kNearPage, MEM_COMMIT, protect)) return nullptr;
        g_near.use[i] = use;
        return reinterpret_cast<void*>(page);
    }
    return nullptr;
}

std::string hex(std::uintptr_t value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}
}  // namespace

extern "C" LPVOID WINAPI al_near_block(ULONG_PTR lowest, ULONG_PTR highest) noexcept {
    return take(lowest, highest, Use::minhook, PAGE_EXECUTE_READWRITE);
}

extern "C" BOOL WINAPI al_near_release(LPVOID block) noexcept {
    const auto page = reinterpret_cast<std::uintptr_t>(block);
    Exclusive hold;
    if (!g_near.base || page < g_near.base || page >= g_near.base + kNearSize) return FALSE;
    // Releasing any page would release the whole reservation; stubs stay.
    auto& use = g_near.use[(page - g_near.base) / kNearPage];
    if (use == Use::minhook && VirtualFree(block, kNearPage, MEM_DECOMMIT)) use = Use::free;
    return TRUE;
}

namespace al::hooks {

std::string reserve_near(const void* image, std::size_t image_size) noexcept {
    try {
        Exclusive hold;
        if (g_near.tried) return "near_memory_unavailable: already reserved in this process";
        g_near.tried = true;
        const auto begin = reinterpret_cast<std::uintptr_t>(image);
        const auto end = begin + image_size;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        if (!image || !image_size || info.dwPageSize != kNearPage)
            return "near_memory_unavailable: no image, or the page size is not 4 KB";
        g_near.image_begin = begin;
        g_near.image_end = end;
        const auto range = window(begin, end, kNearSize);
        const char* side = "below";
        auto* reserved = reserve_below(begin, range, info.dwAllocationGranularity);
        if (!reserved) { side = "above"; reserved = reserve_above(end, range, info.dwAllocationGranularity); }
        if (!reserved)
            return "near_memory_unavailable: no free " + std::to_string(kNearSize / 1024) +
                " KB within reach of the game; hooks and patch stubs search as before";
        g_near.base = reinterpret_cast<std::uintptr_t>(reserved);
        const auto distance = g_near.base < begin ? begin - (g_near.base + kNearSize) : g_near.base - end;
        return "near_memory_reserved base=" + hex(g_near.base) + " size_kb=" + std::to_string(kNearSize / 1024) +
            " side=" + side + " distance_kb=" + std::to_string(distance / 1024);
    } catch (...) { return "near_memory_unavailable: unexpected error"; }
}

void* near_stub_page(std::uintptr_t lowest, std::uintptr_t highest) noexcept {
    return take(lowest, highest, Use::stub, PAGE_READWRITE);
}

NearCounts near_counts() noexcept {
    Exclusive hold;
    NearCounts counts{g_near.base, g_near.base ? kPages : 0, 0, 0};
    for (const auto use : g_near.use) {
        if (use == Use::minhook) ++counts.minhook;
        else if (use == Use::stub) ++counts.stubs;
    }
    return counts;
}

std::string near_summary() noexcept {
    try {
        const auto counts = near_counts();
        std::uintptr_t begin = 0, end = 0;
        { Exclusive hold; begin = g_near.image_begin; end = g_near.image_end; }
        std::string line = "near_memory";
        if (!counts.base) line += " reserved=none";
        else line += " base=" + hex(counts.base) + " pages_used=" + std::to_string(counts.minhook + counts.stubs) + "/" +
            std::to_string(counts.pages) + " minhook_pages=" + std::to_string(counts.minhook) +
            " stub_pages=" + std::to_string(counts.stubs);
        if (begin) line += " free_64kb_within_reach=" + std::to_string(free_units(begin, end));
        return line;
    } catch (...) { return "near_memory summary unavailable"; }
}

}  // namespace al::hooks
