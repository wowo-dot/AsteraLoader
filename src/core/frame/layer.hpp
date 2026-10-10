#pragma once
#include "core/frame/sites.hpp"
#include "core/game_hooks/registry.hpp"
#include <atomic>
#include <functional>
#include <string_view>

namespace al::plugins { class Host; }
namespace al::frame {
// One process-lifetime layer, owned by phase runtime. Callback addresses,
// continuation slots and the registry client stay valid until exit.
class Layer {
public:
    using Log = std::function<void(std::string_view)>;
    using Warning = std::function<void(std::string_view)>;
    Layer(plugins::Host& host, Log log, Warning warning);
    void prepare(Sites sites, bool warn_unavailable);
    void installed(); // after the registry installer has activated pending hooks
    void win_main(std::uint32_t thread) noexcept;
    // Why GAME_READY cannot fire, or "" once its hook is active.
    const std::string& ready_problem() const noexcept { return ready_problem_; }
    // For the core log: whether GAME_READY fired, and how often the title started.
    std::string ready_summary() const;
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
private:
    static std::uintptr_t update(void*, void*, void*, void*);
    static void exiting(void*);
    static std::uintptr_t title(void*, void*, void*, void*);
    void unavailable(std::string reason, bool warn = true) noexcept;
    void ready_unavailable(std::string reason) noexcept;
    void ready() noexcept;
    void notify(std::string_view source) noexcept;
    void note(std::string_view text) noexcept;
    plugins::Host& host_;
    Log log_;
    Warning warning_;
    game_hooks::Client client_;
    Sites sites_;
    std::uint32_t update_handle_{}, exit_handle_{}, title_handle_{};
    void *update_next_{}, *exit_next_{}, *title_next_{};
    std::atomic<bool> armed_{false};
    // GAME_READY: the hook marks the title's start; the frame thread
    // dispatches at that frame's end, once.
    std::atomic<bool> ready_armed_{false}, title_started_{false};
    std::atomic<std::uint32_t> title_starts_{0};
    std::atomic<bool> ready_done_{false}, ready_fired_{false};
    std::string ready_problem_{"not prepared"};
    std::atomic<std::uint32_t> win_main_{0}, frame_thread_{0};
};
}  // namespace al::frame
