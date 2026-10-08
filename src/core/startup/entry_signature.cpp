#include "core/startup/entry_signature.hpp"

#include <array>
#include <cstring>

namespace cmfm::startup {
namespace {
constexpr std::array<std::uint8_t, 18> kWrapper{
    0x48,0x83,0xec,0x28,0xe8,0,0,0,0,0x48,0x83,0xc4,0x28,0xe9,0,0,0,0};

// CRT common-main signature also recorded in SPL's AddressRecords.json.
// Reference: SPL 7be2393, MIT; notice retained under third_party/notices/.
constexpr std::array<std::uint8_t, 22> kCommon{
    0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0xb9,0x01,0,0,0,0xe8,0,0,0,0,0x84,0xc0};
constexpr std::array<std::uint8_t, 8> kDefaultCookie{0x32,0xa2,0xdf,0x2d,0x99,0x2b,0,0};

bool within(std::span<const std::uint8_t> image, std::span<const CodeRange> ranges,
            std::uint32_t rva, std::size_t bytes) {
    if (rva >= image.size() || bytes > image.size() - rva) return false;
    for (const auto& range : ranges) {
        if (rva >= range.rva && rva - range.rva <= range.size && bytes <= range.size - (rva - range.rva)) return true;
    }
    return false;
}

template <std::size_t N>
bool matches(const std::uint8_t* memory, const std::array<std::uint8_t, N>& pattern,
             std::size_t displacement) {
    for (std::size_t i = 0; i < N; ++i) {
        if ((i >= displacement && i < displacement + 4) ||
            (N == 18 && i >= 14 && i < 18)) continue;
        if (memory[i] != pattern[i]) return false;
    }
    return true;
}

std::optional<std::uint32_t> relative_target(const std::uint8_t* memory, std::uint32_t rva,
                                           std::uint32_t end, std::uint32_t displacement) {
    std::int32_t delta = 0;
    std::memcpy(&delta, memory + displacement, sizeof(delta));
    const auto result = std::int64_t{rva} + end + delta;
    if (result < 0 || result > UINT32_MAX) return {};
    return static_cast<std::uint32_t>(result);
}
}  // namespace

std::optional<CrtEntry> find_crt_entry(std::span<const std::uint8_t> image,
                                     std::span<const CodeRange> code, std::string& reason) {
    std::optional<CrtEntry> found;
    for (const auto& range : code) {
        if (range.rva >= image.size() || range.size > image.size() - range.rva || range.size < kWrapper.size()) continue;
        const auto last = range.size - kWrapper.size();
        for (std::size_t offset = 0; offset <= last; ++offset) {
            const auto rva = range.rva + static_cast<std::uint32_t>(offset);
            const auto* memory = image.data() + rva;
            if (memory[0] != 0x48 || !matches(memory, kWrapper, 5)) continue;
            const auto cookie = relative_target(memory, rva, 9, 5);
            const auto common = relative_target(memory, rva, 18, 14);
            if (!cookie || !common || !within(image, code, *cookie, 80) ||
                !within(image, code, *common, kCommon.size()) ||
                !matches(image.data() + *common, kCommon, 16)) continue;
            // Validate a real cookie initializer: load cookie, load MSVC's
            // default literal, compare, and a conditional branch. Never write it.
            const auto* initialiser = image.data() + *cookie;
            bool literal = false;
            for (std::size_t i = 2; i + kDefaultCookie.size() + 5 <= 80; ++i) {
                if ((initialiser[i - 2] == 0x48 || initialiser[i - 2] == 0x49) &&
                    initialiser[i - 1] >= 0xb8 && initialiser[i - 1] <= 0xbf &&
                    !std::memcmp(initialiser + i, kDefaultCookie.data(), kDefaultCookie.size()) &&
                    (initialiser[i + 8] & 0xf8) == 0x48 &&
                    (initialiser[i + 9] == 0x3b || initialiser[i + 9] == 0x39) &&
                    (initialiser[i + 10] & 0xc0) == 0xc0 &&
                    (initialiser[i + 11] == 0x74 || initialiser[i + 11] == 0x75)) {
                    literal = true;
                    break;
                }
            }
            if (!literal) continue;
            if (found) {
                reason = "more than one verified CRT entry; phase skipped";
                return {};
            }
            found = CrtEntry{rva, *cookie, *common};
        }
    }
    reason = found ? "verified unique CRT startup wrapper" : "no verified unpacked CRT startup wrapper";
    return found;
}
}  // namespace cmfm::startup
