#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace al::startup {

struct CodeRange { std::uint32_t rva; std::uint32_t size; };
struct CrtEntry {
    std::uint32_t entry;
    std::uint32_t cookie_initialiser;
    std::uint32_t common_main;
};

// Checks one advisory cached RVA. Every wrapper/target read is bounds checked;
// cookie/common-main targets are derived from the current wrapper, never cached.
std::optional<CrtEntry> verify_crt_entry(std::span<const std::uint8_t> image,
                                       std::span<const CodeRange> code,
                                       std::uint32_t rva, std::string& reason);

// Finds the small MSVC executable startup wrapper, not the common-main body
// SPL hooks. Every read and relative branch must stay in a readable code range.
// A unique match is required. This function performs no writes.
std::optional<CrtEntry> find_crt_entry(std::span<const std::uint8_t> image,
                                     std::span<const CodeRange> code,
                                     std::string& reason);

}  // namespace al::startup
