#pragma once

#include "core/game_hooks/registry.hpp"

namespace cmfm::game_hooks {

// Calls are serialized by Registry. The registry, its head slots, and the
// successfully created entry stubs/trampolines live until process exit.
class NativeInstaller final : public Installer {
public:
    using Log = std::function<void(std::string_view)>;
    explicit NativeInstaller(Log log);
    NativeInstaller(const NativeInstaller&) = delete;
    NativeInstaller& operator=(const NativeInstaller&) = delete;
    std::string create(void* target, void* const* head, void** original) override;
    std::string enable(void* target) override;
private:
    struct State;
    State* state_; // Pinned; no destruction while callers may be in a hook.
};
}  // namespace cmfm::game_hooks
