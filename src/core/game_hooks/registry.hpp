#pragma once

// "al.hooks" (API 1.4), core side: checks each target by its bytes, keeps
// one chain per game function in call order, and relinks the chain when a
// detour joins, leaves or comes back. Patching the function is the hook
// layer's job (Installer); nothing here changes game code.

#include "al/al_plugin.h"
#include "common/pattern.hpp"
#include "core/graphics/hub.hpp"  // Client
#include "core/image/claims.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace al::game_hooks {

using Client = graphics::Client;

// The hook layer for game functions.
class Installer {
public:
    virtual ~Installer() = default;
    // Creates an inactive hook on `target` whose entry jumps through *head.
    // The registry keeps *head at the first enabled detour, or at the
    // original when there is none. Stores in *original the function that
    // runs the target's original code. Returns "" or why it could not.
    virtual std::string create(void* target, void* const* head, void** original) = 0;
    // Activates the hook created for `target`. Returns "" or why it could not.
    virtual std::string enable(void* target) = 0;
};

// Searches the game's code for a pattern (the cached scanner).
using Finder = std::function<std::vector<const void*>(const Pattern&)>;

class Registry {
public:
    // `image` is the game exe; `find` searches its code sections. With
    // `claims`, each chain claims the bytes its hook takes, a hook over
    // another claim is refused, and patterns see the original bytes under
    // al.patch changes (1.8).
    Registry(const void* image, Finder find, image::Claims* claims = nullptr);
    ~Registry();
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Registers one detour; its handle, or 0 with the reason in `problem`.
    std::uint32_t hook(Client& client, const al_hook_t& hook, std::string& problem);
    // "" or why not.
    std::string set_enabled(const Client& client, std::uint32_t handle, bool enabled);
    al_hook_state_t state(const Client& client, std::uint32_t handle) const;

    // The hook layer is ready (after PRE_MAIN): installs every pending chain;
    // later registrations install at once.
    void set_installer(Installer* installer);
    // One line per chain, for the core log.
    std::vector<std::string> summary() const;

private:
    struct Layer;
    struct Chain;

    bool in_code(const void* address, std::size_t size) const;
    // Bytes from `address` to the end of its code section, at most `most`.
    std::size_t code_room(const void* address, std::size_t most) const;
    // AL's patch on the chain's entry is still the one it wrote.
    static bool entry_intact(const Chain& chain);
    bool matches_at(const Pattern& pattern, const std::uint8_t* at) const;
    void install(Chain& chain);
    static void link(Chain& chain);

    const std::uint8_t* image_;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> code_;  // executable sections [begin, end)
    Finder find_;
    image::Claims* claims_ = nullptr;
    mutable std::mutex mutex_;
    Installer* installer_ = nullptr;
    std::vector<std::unique_ptr<Chain>> chains_;
    std::uint32_t next_handle_ = 1;
};

}  // namespace al::game_hooks
