#include "core/startup/entry_signature.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
int failures = 0;
void check(bool value, const char* label) {
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

std::vector<std::uint8_t> fixture() {
    std::vector<std::uint8_t> image(2048, 0xcc);
    const std::array<std::uint8_t, 18> wrapper{0x48,0x83,0xec,0x28,0xe8,0,0,0,0,0x48,0x83,0xc4,0x28,0xe9,0,0,0,0};
    std::memcpy(image.data() + 64, wrapper.data(), wrapper.size());
    const std::int32_t call = 256 - (64 + 9), jump = 512 - (64 + 18);
    std::memcpy(image.data() + 69, &call, sizeof(call));
    std::memcpy(image.data() + 78, &jump, sizeof(jump));
    const std::array<std::uint8_t, 15> cookie{0x48,0xbb,0x32,0xa2,0xdf,0x2d,0x99,0x2b,0,0,0x48,0x3b,0xc3,0x74,0x10};
    std::memcpy(image.data() + 269, cookie.data(), cookie.size());
    const std::array<std::uint8_t, 22> common{0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0xb9,1,0,0,0,0xe8,0,0,0,0,0x84,0xc0};
    std::memcpy(image.data() + 512, common.data(), common.size());
    return image;
}
}  // namespace

int main() {
    auto image = fixture();
    const auto original = image;
    const std::array<cmfm::startup::CodeRange, 1> ranges{{{0, 2048}}};
    std::string reason;
    auto found = cmfm::startup::find_crt_entry(image, ranges, reason);
    check(found && found->entry == 64 && found->cookie_initialiser == 256 && found->common_main == 512,
          "verified relative call and tail jump");
    check(image == original, "signature scan is read-only");
    image[270] = 0xb9; image[281] = 0xc1;
    check(cmfm::startup::find_crt_entry(image, ranges, reason).has_value(), "cookie register allocation can vary");
    image = fixture();
    image[271] ^= 1;
    check(!cmfm::startup::find_crt_entry(image, ranges, reason), "cookie literal mismatch refuses entry");
    image = fixture(); image[512] = 0xe9;
    check(!cmfm::startup::find_crt_entry(image, ranges, reason), "already detoured common-main refused");
    image = fixture();
    const std::int32_t overflow = INT32_MAX;
    std::memcpy(image.data() + 69, &overflow, sizeof(overflow));
    check(!cmfm::startup::find_crt_entry(image, ranges, reason), "call outside image refused");
    image = fixture();
    const std::int32_t negative = -1024;
    std::memcpy(image.data() + 78, &negative, sizeof(negative));
    check(!cmfm::startup::find_crt_entry(image, ranges, reason), "negative branch target refused");
    image = fixture();
    const std::array<cmfm::startup::CodeRange, 1> short_range{{{0, 511}}};
    check(!cmfm::startup::find_crt_entry(image, short_range, reason), "unreadable target refused");
    const std::array<cmfm::startup::CodeRange, 1> bad_range{{{UINT32_MAX, 80}}};
    check(!cmfm::startup::find_crt_entry(image, bad_range, reason), "out-of-bounds range ignored");
    std::memcpy(image.data() + 1024, image.data() + 64, 18);
    const std::int32_t call2 = 256 - (1024 + 9), jump2 = 512 - (1024 + 18);
    std::memcpy(image.data() + 1029, &call2, sizeof(call2));
    std::memcpy(image.data() + 1038, &jump2, sizeof(jump2));
    check(!cmfm::startup::find_crt_entry(image, ranges, reason) && reason.find("more than one") != std::string::npos,
          "ambiguous entry refused");
    check(!cmfm::startup::find_crt_entry({}, {}, reason), "empty image refused");
    if (!failures) std::cout << "Startup signature checks passed\n";
    return failures ? 1 : 0;
}
