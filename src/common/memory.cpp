#include "common/memory.hpp"

#include <windows.h>

#include <cstdint>

namespace cmfm {

bool readable(const void* address, std::size_t size) noexcept {
    auto cursor = reinterpret_cast<std::uintptr_t>(address);
    const auto end = cursor + size;
    if (!address || end < cursor) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info))) return false;
        const auto access = info.Protect & 0xFF;
        if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || access == PAGE_NOACCESS ||
            access == PAGE_EXECUTE) {
            return false;
        }
        const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

}  // namespace cmfm
