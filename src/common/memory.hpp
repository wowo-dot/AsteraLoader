#pragma once

#include <cstddef>

namespace cmfm {

// True if every byte of [address, address + size) is committed memory this
// process may read: not reserved, guard, no-access or execute-only.
bool readable(const void* address, std::size_t size) noexcept;

}  // namespace cmfm
