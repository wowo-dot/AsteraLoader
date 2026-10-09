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
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
private:
    static std::uintptr_t update(void*, void*, void*, void*);
    static void exiting(void*);
    void unavailable(std::string reason, bool warn = true) noexcept;
    void notify(std::string_view source) noexcept;
    void note(std::string_view text) noexcept;
    plugins::Host& host_;
    Log log_;
    Warning warning_;
    game_hooks::Client client_;
    Sites sites_;
    std::uint32_t update_handle_{}, exit_handle_{};
    void *update_next_{}, *exit_next_{};
    std::atomic<bool> armed_{false};
    std::atomic<std::uint32_t> win_main_{0}, frame_thread_{0};
};
}  // namespace al::frame
