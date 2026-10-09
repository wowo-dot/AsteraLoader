#include "common/game_version.hpp"
#include "common/memory.hpp"
#include "common/pattern.hpp"

#include <windows.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* label) {
    if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

void bits() {
    const auto pattern = al::parse_bits(al::kVersionPattern);
    check(pattern.bytes.size() == 9 && pattern.mask.size() == 9, "version pattern is 9 bytes");
    check(pattern.bytes[0] == 0x48 && pattern.mask[0] == 0xF8 && pattern.bytes[1] == 0x8D && pattern.mask[1] == 0xFF &&
          pattern.bytes[3] == 0x24 && pattern.mask[3] == 0x3F && pattern.bytes[8] == 0xF5, "bit notation");
    check(al::parse_bits("0100 1000 x 1").bytes == std::vector<std::uint8_t>{0x48}, "Stracker ignores other characters");
}

void text_patterns() {
    const auto hex = al::parse_pattern("48 8B ?? 05 ? ff");
    check(hex && hex->bytes == std::vector<std::uint8_t>{0x48, 0x8B, 0, 0x05, 0, 0xFF} &&
          hex->mask == std::vector<std::uint8_t>{0xFF, 0xFF, 0, 0xFF, 0, 0xFF}, "hex with wildcards");
    const auto bits = al::parse_pattern("  01001...\t10001101 ");
    check(bits && bits->bytes == std::vector<std::uint8_t>{0x48, 0x8D} &&
          bits->mask == std::vector<std::uint8_t>{0xF8, 0xFF}, "Stracker bits, eight per token");
    for (const char* bad : {"", "   ", "?? ??", "4", "488B", "48 8G", "48 10001101", "01001... 48", "0100100",
                            "48 ??? 05"}) {
        check(!al::parse_pattern(bad), bad);
    }
    std::string long_pattern;
    for (int k = 0; k < 257; ++k) long_pattern += "90 ";
    check(!al::parse_pattern(long_pattern), "longer than 256 bytes");

    check(al::pattern_key(*al::parse_pattern("48 8b ?? 05")) == "488b0005/ffff00ff", "key");
    check(al::pattern_key(*al::parse_pattern("48 8B ? 05")) == al::pattern_key(*al::parse_pattern("48 8b ?? 05")),
          "equal patterns, equal keys");
    check(al::pattern_key(*al::parse_pattern("01001... 10001101")) == "488d/f8ff", "bits key");
}

void search() {
    const auto pattern = al::parse_bits(al::kVersionPattern);
    // Matches at 3 (REX.W variant 0x4F) and 40 (0x48), a near miss at 20.
    const std::uint8_t match_a[] = {0x4F, 0x8D, 0x4C, 0xE4, 0x30, 0x4A, 0x8D, 0x42, 0xF5};
    const std::uint8_t match_b[] = {0x48, 0x8D, 0x4C, 0x24, 0x30, 0x48, 0x8D, 0x42, 0xF5};
    std::vector<std::uint8_t> data(64, 0x8D);
    std::memcpy(data.data() + 3, match_a, sizeof(match_a));
    std::memcpy(data.data() + 20, match_b, sizeof(match_b));
    data[20 + 4] = 0x31;
    std::memcpy(data.data() + 40, match_b, sizeof(match_b));
    std::vector<std::size_t> found;
    al::find_all(data, pattern, 1000, found, 10);
    check(found == std::vector<std::size_t>{1003, 1040}, "masked matches with base offset");
    found.clear();
    al::find_all(data, pattern, 0, found, 1);
    check(found == std::vector<std::size_t>{3}, "match limit");
    found.clear();
    al::find_all({data.data() + 40, 8}, pattern, 0, found, 10);
    check(found.empty(), "pattern longer than the data");
    found.clear();
    al::find_all(std::span<const std::uint8_t>(match_b, sizeof(match_b)), pattern, 0, found, 10);
    check(found == std::vector<std::size_t>{0}, "match filling the whole buffer");
    found.clear();
    const std::uint8_t overlap[] = {0xAA, 0xAA, 0xAA, 0xAA};
    al::find_all(overlap, *al::parse_pattern("AA AA"), 0, found, 10);
    check(found == std::vector<std::size_t>{0, 1, 2}, "overlapping matches");
    found.clear();
    const std::uint8_t loose[] = {0x10, 0x20, 0x30};
    al::find_all(loose, al::parse_bits("0001.... 0010...."), 0, found, 10);
    check(found == std::vector<std::size_t>{0}, "no fully fixed byte");
}

void version_helpers() {
    check(al::version_pointer_address(0x1000, 0x20) == 0x1000 - 0x18 + 7 + 0x20, "pointer, forward");
    check(al::version_pointer_address(0x1000, -0x100) == 0x1000 - 0x18 + 7 - 0x100, "pointer, back");
    check(al::plausible_version("421810") && al::plausible_version("v15.11.01 (421471)"), "plausible versions");
    check(!al::plausible_version("") && !al::plausible_version(std::string(64, '1')) &&
          !al::plausible_version("a\nb"), "implausible versions");
    // This test exe has no MHW version pattern: a reason, no crash.
    const auto none = al::find_game_version(GetModuleHandleW(nullptr));
    check(none.text.empty() && none.failure == "pattern not found", "no version in this exe");
}

void memory() {
    const int value = 1;
    check(al::readable(&value, sizeof(value)), "stack is readable");
    check(!al::readable(nullptr, 1) && !al::readable(reinterpret_cast<const void*>(std::uintptr_t{0x10}), 1),
          "null page is not");
    void* reserved = VirtualAlloc(nullptr, 0x2000, MEM_RESERVE, PAGE_NOACCESS);
    check(reserved && !al::readable(reserved, 1), "reserved memory is not");
    if (reserved) VirtualFree(reserved, 0, MEM_RELEASE);
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD old = 0;
    check(page && VirtualProtect(page + 0x1000, 0x1000, PAGE_NOACCESS, &old), "test pages");
    check(al::readable(page, 0x1000) && !al::readable(page + 0xFFF, 2), "readability ends at the no-access page");
    if (page) VirtualFree(page, 0, MEM_RELEASE);
}
}  // namespace

int main() {
    bits();
    text_patterns();
    search();
    version_helpers();
    memory();
    if (failures) {
        std::cerr << failures << " pattern check(s) failed\n";
        return 1;
    }
    std::cout << "Pattern, search, game-version and memory checks passed\n";
    return 0;
}
