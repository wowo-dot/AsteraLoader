#pragma once

#include "core/patch/registry.hpp"

#include <cstdint>
#include <vector>

namespace al::patch {

// Writes to the game's image in this process:
// - naturally aligned data of 1, 2, 4 or 8 bytes with one interlocked
//   compare-and-swap;
// - anything else with the process's other threads paused, never while one
//   of them is stopped inside the bytes (code only), retrying briefly;
// - read-only pages get write access for the write and their old protection
//   back after it.
// Stubs: one page each, written while read-write, then read-and-execute and
// never written again. Calls are serialized by the registry (the claims'
// write lock); stubs live until the process exits.
class NativeWriter final : public Writer {
public:
    NativeWriter() = default;
    NativeWriter(const NativeWriter&) = delete;
    NativeWriter& operator=(const NativeWriter&) = delete;
    Result write(std::uint8_t* at, const std::uint8_t* expect, const std::uint8_t* bytes, std::size_t size, bool code,
                 std::uint8_t* found, std::string& problem) override;
    std::string stub(const void* site, void* function, void** stub) override;

private:
    struct Region {
        std::uint8_t* base = nullptr;
        std::size_t pages_used = 0;
    };
    std::vector<Region> regions_;
};

}  // namespace al::patch
