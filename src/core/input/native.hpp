#pragma once
#include "core/input/service.hpp"
#include "core/input/sites.hpp"
#include "core/game_hooks/registry.hpp"
#include <functional>
#include <memory>

namespace al::input {
// Pinned until process exit; hooks fall through after EXITING. Captures at
// the game's own input functions through al.hooks, once a plugin wants input.
class Native {
public:
    using Log = std::function<void(std::string_view)>;
    Native(Service&, Log, Log warning);
    // Registers the input hooks now if a plugin already asked for input, or
    // at the first frame after one does.
    void prepare(game_hooks::Registry&, Sites) noexcept;
    void installed() noexcept;  // after the registry's installer is set
    void attach_window(void* hwnd) noexcept;
    void begin_frame(std::uint32_t thread) noexcept;
    void end_frame() noexcept;
private:
    struct State;
    State* state_; // owns continuation slots / window chains until exit
    static std::uintptr_t keyboard_update(void* keyboard);
    static std::uintptr_t mouse_read(void* mouse, void* out);
    static std::uintptr_t pad_update(void* manager, void* slot, const void* state);
    static std::intptr_t window_proc(void*, unsigned, std::uintptr_t, std::intptr_t);
    void hook() noexcept;
    void report() noexcept;
    void fail(std::string_view) noexcept;
};
} // namespace al::input
