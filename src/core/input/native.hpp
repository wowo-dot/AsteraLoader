#pragma once
#include "core/input/service.hpp"
#include <functional>
#include <memory>

namespace al::input {
// Pinned until process exit; hooks fall through after EXITING.
class Native {
public:
    using Log = std::function<void(std::string_view)>;
    Native(Service&, Log, Log warning);
    void attach_window(void* hwnd) noexcept;
    void begin_frame(std::uint32_t thread) noexcept;
    void end_frame() noexcept;
    struct Methods { void* state[2]{}; void* data[2]{}; void* pads[4]{}; };
    // Internal seam for native stand-in tests, never exposed to plugins.
    bool install(const Methods&) noexcept;
private:
    struct State;
    State* state_; // owns stable continuation slots / window chains until exit
    static long state_read(unsigned slot, void*, unsigned long, void*);
    static long data_read(unsigned slot, void*, unsigned long, void*, unsigned long*, unsigned long);
    static unsigned long pad_read(unsigned slot, unsigned long, void*);
    static std::intptr_t window_proc(void*, unsigned, std::uintptr_t, std::intptr_t);
    void discover() noexcept;
    void fail(std::string_view) noexcept;
};
} // namespace al::input
