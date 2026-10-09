#pragma once

// Byte patterns for finding game code, and a fast masked search.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace al {

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;  // per byte: 0xFF must match, 0x00 any, other values per bit
};

// Stracker's bit notation, read the way Stracker reads it: '0' and '1' are
// bits, '.' is a wildcard bit, anything else is ignored, and a trailing
// partial byte is dropped.
Pattern parse_bits(std::string_view bits);

// A pattern as plugins write it, tokens separated by spaces:
// - hex bytes, with "?" or "??" for any byte: "48 8B ?? 05";
// - Stracker's bits, eight per token: "01001... 10001101".
// The two forms do not mix. Empty, all-wildcard or longer-than-256-byte
// patterns are invalid.
std::optional<Pattern> parse_pattern(std::string_view text);

// The same text for equal patterns: "488b0005/ffff00ff" (masked bytes/mask).
std::string pattern_key(const Pattern& pattern);

// Appends to `found` the offset, plus `base`, of every (overlapping) match in
// `data`, up to `limit` matches in total.
void find_all(std::span<const std::uint8_t> data, const Pattern& pattern, std::size_t base,
              std::vector<std::size_t>& found, std::size_t limit);

}  // namespace al
