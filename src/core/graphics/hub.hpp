#pragma once

// "al.d3d12" (API 1.3), core side: who registered what, the call order of
// every hooked method, and the events AL's own layer raises. Putting hooks
// on methods is the hook layer's job (Installer); nothing here patches code.

#include "al/al_plugin.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace al::graphics {

// A plugin as the hub sees it. The host makes one per plugin that uses the
// service and keeps it until the process exits.
struct Client {
    al_plugin_t* self = nullptr;
    std::string name;
    std::size_t load_index = 0;
    const std::atomic<bool>* stopped = nullptr;  // the plugin's own flag: true skips its callbacks
    std::function<void(std::string_view level, std::string_view text)> log;
    std::function<void(std::string reason)> disable;  // one of its callbacks faulted
};

// One detour in a chain.
struct Layer {
    void* detour = nullptr;
    void** next = nullptr;           // the hook layer stores the function to continue with here
    const Client* client = nullptr;  // null for AL's own event layer
    std::int32_t order = 0;
};

// Every detour on one method, in call order: the first layer is entered
// first; the last layer's next is the original method.
struct Chain {
    al_d3d12_method_t method = 0;
    std::vector<Layer> layers;
};

// The hook layer. It puts one hook on each chain's method and fills every
// layer's next: each layer continues with the following one, the last with
// the original. Its entry goes straight to the original while internal() is
// true for the calling thread.
class Hub;
class Installer {
public:
    virtual ~Installer() = default;
    // Hooks `chains` (all of one family) on the class of `object`: for
    // SWAPCHAIN the game's swap chain; for FACTORY, DEVICE, QUEUE and
    // COMMAND_LIST the game's device, from which the hook layer finds or
    // makes an object of that family. Returns "" or why it could not; then
    // no layer's next may have been written.
    virtual std::string install(std::uint32_t family, void* object, const std::vector<const Chain*>& chains) = 0;
    // Called after the hub has recorded an installation's result. No plugin
    // ABI is involved; pure-logic stand-ins need not produce diagnostics.
    virtual void report(const Hub&) noexcept {}
};

// Calls of the game's present thread, as AL's layer sees them.
struct PresentCall {
    bool raised = false;  // events were raised: end_present raises POST_PRESENT
    al_d3d12_present_t data{};
};

struct ResizeCall {
    bool raised = false;
    al_d3d12_resize_t data{};
};

class Hub {
public:
    Hub();
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    // Registration, until close(). Each returns "" or the reason it refused,
    // for the caller to log.
    std::string subscribe(Client& client, std::uint32_t events, std::int32_t order, al_d3d12_event_fn callback,
                          void* user);
    std::string hook(Client& client, const al_d3d12_hook_t* hooks, std::uint32_t count);
    std::string claim_presentation(Client& client);
    void status(al_d3d12_status_t& out) const;

    // The host, after PRE_MAIN: orders every chain and adds AL's own layer
    // where events or the phases in `phase_mask` (the plugins' on_phase
    // subscriptions) need it. Registration is refused from then on.
    void close(std::uint32_t phase_mask);
    bool closed() const;
    const std::vector<Chain>& chains() const;  // after close(); empty when nobody asked
    bool wants(std::uint32_t family) const;    // any chain of that family
    void set_installer(Installer* installer);
    void set_phase_dispatch(std::function<void(al_phase_t, const void*)> dispatch);
    // Nothing will be called, for good; status() reports the reason.
    void set_unavailable(std::string reason);
    // The game's device exists (DEVICE_CREATED): hooks the factory, device,
    // queue and command-list chains.
    void device_created(void* device);
    // One line per subscription and chain, for the core log.
    std::vector<std::string> summary() const;

    // AL's own layer (layers.cpp).
    void swapchain_created(void* swapchain, void* queue);
    PresentCall begin_present(void* swapchain, std::uint32_t sync_interval, std::uint32_t flags);
    void end_present(PresentCall& call, std::int32_t result);
    ResizeCall begin_resize(void* swapchain, std::uint32_t buffer_count, std::uint32_t width, std::uint32_t height,
                            std::uint32_t format, std::uint32_t flags);
    void end_resize(ResizeCall& call, std::int32_t result);

private:
    struct Subscription {
        Client* client = nullptr;
        std::uint32_t events = 0;
        std::int32_t order = 0;
        al_d3d12_event_fn callback = nullptr;
        void* user = nullptr;
        std::array<std::atomic<std::uint64_t>, AL_D3D12_EVENT_COUNT> calls{};
        std::array<std::atomic<std::uint64_t>, AL_D3D12_EVENT_COUNT> ticks{};
        std::array<std::atomic<std::uint64_t>, AL_D3D12_EVENT_COUNT> max_ticks{};
    };

    void raise(al_d3d12_event_t event, const void* data);
    void install(std::uint32_t family, void* object);
    void describe(void* swapchain);
    bool is_present_thread();

    mutable std::mutex mutex_;  // registration and status strings
    std::mutex install_mutex_;  // serialize first installation from concurrent creation calls
    std::atomic<bool> closed_{false};
    std::vector<std::unique_ptr<Subscription>> subscriptions_;
    std::vector<Layer> pending_;  // plugin detours with their methods, until close
    std::vector<al_d3d12_method_t> pending_methods_;
    const Client* owner_ = nullptr;
    std::string owner_name_;

    // Fixed after close(): read without locks.
    std::vector<Chain> chains_;
    std::array<std::vector<Subscription*>, AL_D3D12_EVENT_COUNT> by_event_{};
    std::uint32_t phase_mask_ = 0;
    Installer* installer_ = nullptr;
    std::function<void(al_phase_t, const void*)> dispatch_phase_;
    std::array<std::atomic<bool>, 8> installed_{};  // per family, also read by summary

    std::atomic<al_d3d12_state_t> state_{AL_D3D12_WAITING};
    std::string reason_;  // set once, with state_ UNAVAILABLE

    // The game's swap chain, updated on creation and resize.
    al_d3d12_swapchain_t current_{};
    void* swapchain3_ = nullptr;  // IDXGISwapChain3 of current_, for the back-buffer index
    void* device_ = nullptr;
    bool swapchain_phase_done_ = false;
    std::atomic<std::uint32_t> present_thread_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::int64_t> last_present_qpc_{0};
};

// AL's own layer. layers.cpp keeps the hub it reports to.
void set_layer_hub(Hub* hub) noexcept;
void release_layer_hub(Hub* hub) noexcept;
// The detour functions of AL's layer, for a method, or null.
void* layer_detour(al_d3d12_method_t method) noexcept;
void** layer_next(al_d3d12_method_t method) noexcept;

// Internal-call regions (begin_internal / end_internal), per thread.
void begin_internal() noexcept;
void end_internal() noexcept;
// For the hook layer's entry: true sends the call straight to the original.
bool internal() noexcept;

}  // namespace al::graphics
