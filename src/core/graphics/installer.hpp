#pragma once

#include "core/graphics/hub.hpp"

namespace cmfm::graphics {

// The core and its installer live until exit. Successfully created hooks,
// trampolines and entry contexts are pinned, including a failed activation:
// another thread may already be inside an entry when activation rolls back.
class NativeInstaller final : public Installer {
public:
    using Log = std::function<void(std::string_view)>;
    using Warning = std::function<void(std::string_view)>;
    NativeInstaller(Log log, Warning warning);
    NativeInstaller(const NativeInstaller&) = delete;
    NativeInstaller& operator=(const NativeInstaller&) = delete;
    std::string install(std::uint32_t family, void* object,
                        const std::vector<const Chain*>& chains) override;
    void report(const Hub& hub) noexcept override;
private:
    struct State;
    State* state_;
};

namespace detail {
// Minimum SDK interface version containing the slot, or -1. These bounds
// come from the SDK's C vtable layouts, rather than a guessed slot limit.
int interface_version(std::uint32_t family, std::uint32_t slot) noexcept;
// An x64 ABI-transparent tail bridge. Kept separate so the register/stack
// contract can also be exercised with mixed floating-point arguments.
void* entry_stub(void* context, void* (*select)(void*) noexcept);
}
}  // namespace cmfm::graphics
