#include "common/pattern.hpp"

#include <cstring>

namespace al {
namespace {
constexpr std::size_t kMaxPatternBytes = 256;

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool bit_token(std::string_view token) {
    if (token.size() != 8) return false;
    for (const char c : token) {
        if (c != '0' && c != '1' && c != '.') return false;
    }
    return true;
}
}  // namespace

Pattern parse_bits(std::string_view bits) {
    Pattern pattern;
    std::uint8_t byte = 0, mask = 0;
    int count = 0;
    for (const char c : bits) {
        if (c != '0' && c != '1' && c != '.') continue;
        byte = static_cast<std::uint8_t>((byte << 1) | (c == '1'));
        mask = static_cast<std::uint8_t>((mask << 1) | (c != '.'));
        if (++count == 8) {
            pattern.bytes.push_back(byte);
            pattern.mask.push_back(mask);
            byte = mask = 0;
            count = 0;
        }
    }
    return pattern;
}

std::optional<Pattern> parse_pattern(std::string_view text) {
    Pattern pattern;
    enum class Form { unknown, hex, bits } form = Form::unknown;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        const auto end = text.find_first_of(" \t", i);
        const auto token = text.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
        i += token.size();
        const auto token_form = bit_token(token) ? Form::bits : Form::hex;
        if (form != Form::unknown && token_form != form) return std::nullopt;
        form = token_form;
        if (form == Form::bits) {
            const auto byte = parse_bits(token);
            pattern.bytes.push_back(byte.bytes[0]);
            pattern.mask.push_back(byte.mask[0]);
        } else if (token == "?" || token == "??") {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(0);
        } else if (token.size() == 2 && hex_digit(token[0]) >= 0 && hex_digit(token[1]) >= 0) {
            pattern.bytes.push_back(static_cast<std::uint8_t>(hex_digit(token[0]) * 16 + hex_digit(token[1])));
            pattern.mask.push_back(0xFF);
        } else {
            return std::nullopt;
        }
        if (pattern.bytes.size() > kMaxPatternBytes) return std::nullopt;
    }
    bool fixed = false;
    for (const auto m : pattern.mask) fixed = fixed || m != 0;
    if (!fixed) return std::nullopt;
    return pattern;
}

std::string pattern_key(const Pattern& pattern) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    for (std::size_t k = 0; k < pattern.bytes.size(); ++k) {
        const auto byte = static_cast<std::uint8_t>(pattern.bytes[k] & pattern.mask[k]);
        key += kHex[byte >> 4];
        key += kHex[byte & 15];
    }
    key += '/';
    for (const auto m : pattern.mask) {
        key += kHex[m >> 4];
        key += kHex[m & 15];
    }
    return key;
}

void find_all(std::span<const std::uint8_t> data, const Pattern& pattern, std::size_t base,
              std::vector<std::size_t>& found, std::size_t limit) {
    const auto size = pattern.bytes.size();
    if (!size || pattern.mask.size() != size || data.size() < size) return;
    const auto matches = [&](std::size_t at) {
        for (std::size_t k = 0; k < size; ++k) {
            if ((data[at + k] & pattern.mask[k]) != (pattern.bytes[k] & pattern.mask[k])) return false;
        }
        return true;
    };
    // Candidates come from memchr on the last fully fixed byte.
    std::size_t anchor = size;
    for (std::size_t k = size; k-- > 0;) {
        if (pattern.mask[k] == 0xFF) {
            anchor = k;
            break;
        }
    }
    if (anchor == size) {
        for (std::size_t at = 0; at + size <= data.size() && found.size() < limit; ++at) {
            if (matches(at)) found.push_back(base + at);
        }
        return;
    }
    const auto* begin = data.data();
    const auto last = data.size() - size + anchor;  // last place the anchor byte can be
    auto from = anchor;
    while (from <= last && found.size() < limit) {
        const auto* hit = static_cast<const std::uint8_t*>(std::memchr(begin + from, pattern.bytes[anchor], last - from + 1));
        if (!hit) break;
        const auto at = static_cast<std::size_t>(hit - begin) - anchor;
        if (matches(at)) found.push_back(base + at);
        from = static_cast<std::size_t>(hit - begin) + 1;
    }
}

}  // namespace al
