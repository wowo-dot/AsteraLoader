#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cmfm {

// SHA-256 (FIPS 180-4) in plain C++, so CMFM needs no crypto DLL to identify
// files in its logs.
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();
    void update(const void* data, std::size_t size);
    Digest finish();

    static std::wstring hex(const Digest& digest);

private:
    void block(const std::uint8_t* data);

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

// Lower-case hex SHA-256 of a file, or an empty string if it cannot be read.
std::wstring sha256_file(const std::wstring& path);

}  // namespace cmfm
