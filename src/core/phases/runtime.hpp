#pragma once
#include "core/plugins/host.hpp"
#include <string_view>

namespace al::phases {
using Log = void (*)(std::wstring_view);
using Warning = void (*)(std::wstring_view) noexcept;
// Called on the startup thread after PRE_MAIN. No disk patching, no hooks
// on D3D12/DXGI exports, and no phase/per-frame polling after the first device.
void arm(plugins::Host& host, Log log, Warning warning, const void* crt_wrapper) noexcept;
}  // namespace al::phases
