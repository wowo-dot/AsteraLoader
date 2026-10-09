#pragma once
#include "core/graphics/hub.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace al::input {
using Client = graphics::Client;
class Service {
public:
    Service();
    ~Service();
    std::uint32_t add(Client&, std::uint32_t key, std::uint32_t modifiers, const char* name,
                      al_hotkey_fn, void*, std::string& problem, int pad = -1);
    std::string remove(const Client&, std::uint32_t handle);
    std::string messages(Client&, std::int32_t order, al_message_fn, void*);
    std::string capture(Client&, std::uint32_t devices);
    std::string release(Client&);
    void cancel(const Client&) noexcept;
    bool key_down(std::uint32_t key) const noexcept;
    bool pad_state(std::uint32_t slot, al_pad_state_t&) const noexcept;
    std::uint32_t capture_mask() const noexcept;
    bool wanted() const noexcept { return wanted_.load(); }
    void request() noexcept { wanted_.store(true); }
    void window(void* hwnd) noexcept { window_.store(hwnd); }
    void* window() const noexcept { return window_.load(); }
    void focus(bool focused) noexcept;
    bool focused() const noexcept { return focused_.load(); }
    void ready(std::uint32_t devices) noexcept;
    void unavailable(std::string reason);
    void stop() noexcept;
    void status(al_input_status_t&) const;
    std::vector<std::string> summary() const;
    // Called only for device reads by the frame thread, before neutralizing.
    void keyboard(const std::uint8_t*, const std::array<std::uint8_t, 256>& map) noexcept;
    void mouse(const std::uint8_t* buttons, unsigned count) noexcept;
    void pad(std::uint32_t slot, const al_pad_state_t&) noexcept;
    void end_frame() noexcept;
    void set_native(std::function<void(void*)> window, std::function<void(std::uint32_t)> begin);
    void attach_window(void* hwnd);
    void begin_frame(std::uint32_t thread) noexcept;
    bool message(void* hwnd, std::uint32_t, std::uintptr_t, std::intptr_t) noexcept;
private:
    struct Hotkey;
    struct Message;
    struct Capture;
    struct List;
    struct Pad {
        std::atomic<std::uint32_t> sequence{0};
        std::atomic<std::uint64_t> a{0}, b{0};
        std::atomic<bool> connected{false};
        std::atomic<std::uint64_t> epoch{0};
    };
    Capture& owner(Client&); // under mutex
    void publish(); // under mutex
    bool other_capture(const Client&) const noexcept;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Hotkey>> keys_;
    std::vector<std::unique_ptr<Message>> messages_;
    std::vector<std::unique_ptr<Capture>> captures_;
    std::vector<std::unique_ptr<List>> kept_;
    std::atomic<const List*> list_{nullptr};
    std::array<std::atomic<std::uint64_t>, 4> keyboard_{};
    std::atomic<std::uint32_t> mouse_{0};
    std::atomic<std::uint64_t> focus_epoch_{1}, keyboard_epoch_{0}, mouse_epoch_{0};
    std::array<Pad, 4> pads_{};
    std::atomic<void*> window_{nullptr};
    std::atomic<bool> wanted_{false}, focused_{false};
    std::atomic<std::uint32_t> devices_{0}, state_{AL_INPUT_WAITING};
    std::string reason_;
    std::uint32_t next_handle_{1};
    std::uint64_t edge_epoch_{0}; // frame thread, suppress focus-change edges
    std::atomic<std::uint64_t> fired_{0}, consumed_{0};
    std::function<void(void*)> attach_;
    std::function<void(std::uint32_t)> begin_;
};
} // namespace al::input
