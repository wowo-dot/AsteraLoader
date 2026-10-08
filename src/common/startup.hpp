#pragma once

#include <cstdint>

namespace cmfm {

// Private launcher/core handshake, not the public plugin API. All fields are
// fixed-width; the remote entry uses the Windows x64 thread calling convention.
inline constexpr char kBootstrapExport[] = "cmfm_bootstrap";
inline constexpr std::uint32_t kStartupAbi = 1;
inline constexpr std::uint32_t kStartupOk = 0x434d464d;

struct StartupRequest {
    std::uint32_t size = sizeof(StartupRequest);
    std::uint32_t abi = kStartupAbi;
    std::uint64_t launcher_qpc = 0;
};

static_assert(sizeof(StartupRequest) == 16);

}  // namespace cmfm
