#include "core/graphics/hub.hpp"

#include "core/plugins/guard.hpp"

#include <windows.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>

namespace al::graphics {
namespace {
thread_local int t_internal = 0;

constexpr std::uint32_t kFamilyCount = AL_D3D12_COMMAND_LIST + 1;

const char* family_name(std::uint32_t family) {
    switch (family) {
    case AL_D3D12_FACTORY: return "factory";
    case AL_D3D12_SWAPCHAIN: return "swapchain";
    case AL_D3D12_DEVICE: return "device";
    case AL_D3D12_QUEUE: return "queue";
    case AL_D3D12_COMMAND_LIST: return "command_list";
    default: return "unknown";
    }
}

const char* event_name(std::uint32_t event) {
    switch (event) {
    case AL_D3D12_SWAPCHAIN_CREATED: return "swapchain_created";
    case AL_D3D12_PRE_PRESENT: return "pre_present";
    case AL_D3D12_POST_PRESENT: return "post_present";
    case AL_D3D12_PRE_RESIZE: return "pre_resize";
    case AL_D3D12_POST_RESIZE: return "post_resize";
    default: return "unknown";
    }
}

std::string method_name(al_d3d12_method_t method) {
    return std::string(family_name(AL_D3D12_METHOD_FAMILY(method))) + ":" +
           std::to_string(AL_D3D12_METHOD_SLOT(method));
}

bool presentation_family(std::uint32_t family) {
    return family == AL_D3D12_FACTORY || family == AL_D3D12_SWAPCHAIN;
}

std::int64_t qpc() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double ticks_to_us(std::uint64_t ticks) {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return frequency > 0 ? static_cast<double>(ticks) * 1e6 / frequency : 0.0;
}

void note(const Client& client, std::string_view level, std::string_view text) {
    if (client.log) client.log(level, text);
}
}  // namespace

void begin_internal() noexcept {
    ++t_internal;
}

void end_internal() noexcept {
    if (t_internal > 0) --t_internal;
}

bool internal() noexcept {
    return t_internal > 0;
}

Hub::Hub() {
    current_.size = sizeof(current_);
}

Hub::~Hub() {
    release_layer_hub(this);
}

std::string Hub::subscribe(Client& client, std::uint32_t events, std::int32_t order, al_d3d12_event_fn callback,
                           void* user) {
    constexpr std::uint32_t known = (1u << AL_D3D12_EVENT_COUNT) - 1;
    std::lock_guard lock(mutex_);
    if (closed_) return "registration closed when PRE_MAIN ended";
    if (!callback) return "no callback";
    if (!events || (events & ~known)) return "the event mask is empty or names unknown events";
    auto subscription = std::make_unique<Subscription>();
    subscription->client = &client;
    subscription->events = events;
    subscription->order = order;
    subscription->callback = callback;
    subscription->user = user;
    subscriptions_.push_back(std::move(subscription));
    return {};
}

std::string Hub::hook(Client& client, const al_d3d12_hook_t* hooks, std::uint32_t count) {
    std::lock_guard lock(mutex_);
    if (closed_) return "registration closed when PRE_MAIN ended";
    if (!hooks || !count) return "no hooks";
    std::vector<al_d3d12_method_t> mine;
    for (std::size_t k = 0; k < pending_.size(); ++k) {
        if (pending_[k].client == &client) mine.push_back(pending_methods_[k]);
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        const auto& hook = hooks[k];
        const auto family = AL_D3D12_METHOD_FAMILY(hook.method);
        const auto slot = AL_D3D12_METHOD_SLOT(hook.method);
        const auto shown = "hook " + std::to_string(k) + " (" + method_name(hook.method) + ")";
        if (family < AL_D3D12_FACTORY || family > AL_D3D12_COMMAND_LIST) return shown + ": unknown family";
        if (slot < 3) return shown + ": IUnknown methods cannot be hooked";
        if (slot >= AL_D3D12_SLOT_LIMIT) return shown + ": slot beyond AL_D3D12_SLOT_LIMIT";
        if (!hook.detour || !hook.next) return shown + ": no detour or no next";
        if (presentation_family(family) && owner_ != &client) {
            return shown + ": " + family_name(family) + " methods need the presentation role" +
                   (owner_ ? ", which " + owner_name_ + " holds" : "; claim it first");
        }
        if (std::find(mine.begin(), mine.end(), hook.method) != mine.end()) return shown + ": hooked twice";
        mine.push_back(hook.method);
    }
    for (std::uint32_t k = 0; k < count; ++k) {
        *hooks[k].next = nullptr;
        pending_.push_back({hooks[k].detour, hooks[k].next, &client, hooks[k].order});
        pending_methods_.push_back(hooks[k].method);
    }
    return {};
}

std::string Hub::claim_presentation(Client& client) {
    std::lock_guard lock(mutex_);
    if (closed_) return "registration closed when PRE_MAIN ended";
    if (owner_ == &client) return {};
    if (owner_) return owner_name_ + " already holds the presentation role";
    owner_ = &client;
    owner_name_ = client.name;
    return {};
}

void Hub::status(al_d3d12_status_t& out) const {
    if (out.size < sizeof(std::uint32_t)) return;
    al_d3d12_status_t full{};
    full.size = sizeof(full);
    full.state = state_.load();
    {
        std::lock_guard lock(mutex_);
        full.reason = full.state == AL_D3D12_UNAVAILABLE ? reason_.c_str() : nullptr;
        full.owner = owner_ ? owner_name_.c_str() : nullptr;
    }
    full.frames = frames_.load(std::memory_order_relaxed);
    full.last_present_qpc = last_present_qpc_.load(std::memory_order_relaxed);
    const auto size = std::min<std::size_t>(out.size, sizeof(full));
    std::memcpy(&out, &full, size);
    out.size = static_cast<std::uint32_t>(size);
}

void Hub::close(std::uint32_t phase_mask) {
    std::lock_guard lock(mutex_);
    if (closed_) return;
    phase_mask_ = phase_mask;

    // Subscriptions per event: lower order first, then load order.
    for (std::uint32_t event = 0; event < AL_D3D12_EVENT_COUNT; ++event) {
        auto& list = by_event_[event];
        for (const auto& subscription : subscriptions_) {
            if (subscription->events & AL_D3D12_EVENT_BIT(event)) list.push_back(subscription.get());
        }
        std::stable_sort(list.begin(), list.end(), [](const Subscription* a, const Subscription* b) {
            return a->order != b->order ? a->order < b->order : a->client->load_index < b->client->load_index;
        });
    }

    // Plugin detours per method, in the same order.
    std::map<al_d3d12_method_t, std::vector<Layer>> methods;
    for (std::size_t k = 0; k < pending_.size(); ++k) methods[pending_methods_[k]].push_back(pending_[k]);
    for (auto& [method, layers] : methods) {
        std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) {
            return a.order != b.order ? a.order < b.order : a.client->load_index < b.client->load_index;
        });
    }

    // AL's own layer, entered first, only where something needs it.
    const auto subscribed = [&](std::uint32_t event) { return !by_event_[event].empty(); };
    const bool present = subscribed(AL_D3D12_PRE_PRESENT) || subscribed(AL_D3D12_POST_PRESENT) ||
                         (phase_mask & AL_PHASE_BIT(AL_PHASE_FIRST_PRESENT));
    const bool resize = subscribed(AL_D3D12_PRE_RESIZE) || subscribed(AL_D3D12_POST_RESIZE);
    bool swapchain_detours = false;
    for (const auto& [method, layers] : methods) {
        if (AL_D3D12_METHOD_FAMILY(method) == AL_D3D12_SWAPCHAIN) swapchain_detours = true;
    }
    // Swap-chain detours are hooked when the game creates its swap chain,
    // which AL's factory layer sees.
    const bool creation = present || resize || swapchain_detours || subscribed(AL_D3D12_SWAPCHAIN_CREATED) ||
                          (phase_mask & AL_PHASE_BIT(AL_PHASE_SWAPCHAIN_CREATED));
    std::vector<al_d3d12_method_t> own;
    if (creation) {
        own.push_back(AL_D3D12_FACTORY_CREATE_SWAPCHAIN);
        own.push_back(AL_D3D12_FACTORY_CREATE_SWAPCHAIN_FOR_HWND);
    }
    if (present) {
        own.push_back(AL_D3D12_SWAPCHAIN_PRESENT);
        own.push_back(AL_D3D12_SWAPCHAIN_PRESENT1);
    }
    if (resize) {
        own.push_back(AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS);
        own.push_back(AL_D3D12_SWAPCHAIN_RESIZE_BUFFERS1);
    }
    for (const auto method : own) {
        auto& layers = methods[method];
        layers.insert(layers.begin(), Layer{layer_detour(method), layer_next(method), nullptr, INT_MIN});
        *layer_next(method) = nullptr;
    }
    if (!own.empty()) set_layer_hub(this);

    for (auto& [method, layers] : methods) chains_.push_back({method, std::move(layers)});
    closed_ = true;
}

bool Hub::closed() const {
    return closed_;
}

const std::vector<Chain>& Hub::chains() const {
    return chains_;
}

bool Hub::wants(std::uint32_t family) const {
    return std::any_of(chains_.begin(), chains_.end(),
                       [&](const Chain& chain) { return AL_D3D12_METHOD_FAMILY(chain.method) == family; });
}

void Hub::set_installer(Installer* installer) {
    installer_ = installer;
}

void Hub::set_phase_dispatch(std::function<void(al_phase_t, const void*)> dispatch) {
    dispatch_phase_ = std::move(dispatch);
}

void Hub::set_unavailable(std::string reason) {
    std::vector<const Client*> told;
    {
        std::lock_guard lock(mutex_);
        if (state_ == AL_D3D12_UNAVAILABLE) return;
        reason_ = std::move(reason);
        state_ = AL_D3D12_UNAVAILABLE;
        for (const auto& subscription : subscriptions_) told.push_back(subscription->client);
        for (const auto& layer : pending_) told.push_back(layer.client);
        if (owner_) told.push_back(owner_);
    }
    std::sort(told.begin(), told.end());
    told.erase(std::unique(told.begin(), told.end()), told.end());
    for (const auto* client : told) note(*client, "WARN", "al.d3d12 is unavailable: " + reason_);
}

void Hub::install(std::uint32_t family, void* object) {
    std::lock_guard install_lock(install_mutex_);
    if (family >= kFamilyCount || installed_[family] || state_ == AL_D3D12_UNAVAILABLE || !object) return;
    std::vector<const Chain*> list;
    for (const auto& chain : chains_) {
        if (AL_D3D12_METHOD_FAMILY(chain.method) == family) list.push_back(&chain);
    }
    if (list.empty()) return;
    if (!installer_) {
        set_unavailable("this Astera Loader build cannot hook Direct3D 12 methods yet");
        return;
    }
    const auto problem = installer_->install(family, object, list);
    if (!problem.empty()) {
        set_unavailable(std::string("cannot hook ") + family_name(family) + " methods: " + problem);
        installer_->report(*this);
        return;
    }
    installed_[family] = true;
    auto expected = AL_D3D12_WAITING;
    state_.compare_exchange_strong(expected, AL_D3D12_RUNNING);
    installer_->report(*this);
}

void Hub::device_created(void* device) {
    if (!closed_ || !device) return;
    device_ = device;
    current_.device = device;
    for (const auto family : {AL_D3D12_DEVICE, AL_D3D12_QUEUE, AL_D3D12_COMMAND_LIST, AL_D3D12_FACTORY}) {
        install(family, device);
    }
}

void Hub::describe(void* swapchain) {
    auto* unknown = static_cast<IUnknown*>(swapchain);
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&chain1)))) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (SUCCEEDED(chain1->GetDesc1(&desc))) {
            current_.width = desc.Width;
            current_.height = desc.Height;
            current_.format = static_cast<std::uint32_t>(desc.Format);
            current_.buffer_count = desc.BufferCount;
            current_.flags = desc.Flags;
        }
        HWND window = nullptr;
        if (SUCCEEDED(chain1->GetHwnd(&window))) current_.window = window;
        chain1->Release();
    }
    // A plain pointer: the game's reference keeps the object alive, and a
    // reference held here would keep its window from taking a new swap chain.
    IDXGISwapChain3* chain3 = nullptr;
    swapchain3_ = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&chain3)))) {
        swapchain3_ = chain3;
        chain3->Release();
    }
}

void Hub::swapchain_created(void* swapchain, void* queue) {
    if (!swapchain || state_ == AL_D3D12_UNAVAILABLE) return;
    current_.swapchain = swapchain;
    current_.queue = queue;
    current_.device = device_;
    describe(swapchain);
    // Detours go in before anyone hears of the swap chain.
    install(AL_D3D12_SWAPCHAIN, swapchain);
    if (state_ == AL_D3D12_UNAVAILABLE) return;
    if (!swapchain_phase_done_) {
        swapchain_phase_done_ = true;
        if (dispatch_phase_) dispatch_phase_(AL_PHASE_SWAPCHAIN_CREATED, &current_);
    }
    raise(AL_D3D12_SWAPCHAIN_CREATED, &current_);
}

bool Hub::is_present_thread() {
    const auto self = static_cast<std::uint32_t>(GetCurrentThreadId());
    auto expected = 0u;
    if (present_thread_.compare_exchange_strong(expected, self)) return true;
    return expected == self;
}

PresentCall Hub::begin_present(void* swapchain, std::uint32_t sync_interval, std::uint32_t flags) {
    PresentCall call;
    if (state_ == AL_D3D12_UNAVAILABLE || !swapchain || swapchain != current_.swapchain || !is_present_thread()) return call;
    const auto now = qpc();
    auto& data = call.data;
    data.size = sizeof(data);
    data.swapchain = &current_;
    data.frame = frames_.load(std::memory_order_relaxed);
    data.sync_interval = sync_interval;
    data.flags = flags;
    data.present_qpc = now;
    data.previous_qpc = last_present_qpc_.load(std::memory_order_relaxed);
    if (swapchain3_ && !by_event_[AL_D3D12_PRE_PRESENT].empty()) {
        data.back_buffer = static_cast<IDXGISwapChain3*>(swapchain3_)->GetCurrentBackBufferIndex();
    }
    if (data.frame == 0 && dispatch_phase_) dispatch_phase_(AL_PHASE_FIRST_PRESENT, &data);
    raise(AL_D3D12_PRE_PRESENT, &data);
    last_present_qpc_.store(now, std::memory_order_relaxed);
    call.raised = true;
    return call;
}

void Hub::end_present(PresentCall& call, std::int32_t result) {
    if (!call.raised) return;
    call.data.result = result;
    raise(AL_D3D12_POST_PRESENT, &call.data);
    frames_.fetch_add(1, std::memory_order_relaxed);
}

ResizeCall Hub::begin_resize(void* swapchain, std::uint32_t buffer_count, std::uint32_t width, std::uint32_t height,
                             std::uint32_t format, std::uint32_t flags) {
    ResizeCall call;
    if (state_ == AL_D3D12_UNAVAILABLE || !swapchain || swapchain != current_.swapchain) return call;
    call.data = {sizeof(call.data), 0, &current_, buffer_count, width, height, format, flags, 0};
    raise(AL_D3D12_PRE_RESIZE, &call.data);
    call.raised = true;
    return call;
}

void Hub::end_resize(ResizeCall& call, std::int32_t result) {
    if (!call.raised) return;
    if (result >= 0) describe(current_.swapchain);
    call.data.result = result;
    raise(AL_D3D12_POST_RESIZE, &call.data);
}

void Hub::raise(al_d3d12_event_t event, const void* data) {
    if (state_ == AL_D3D12_UNAVAILABLE) return;
    for (auto* subscription : by_event_[event]) {
        auto& client = *subscription->client;
        if (client.stopped && client.stopped->load(std::memory_order_relaxed)) continue;
        unsigned long fault = 0;
        const auto start = qpc();
        const bool returned =
            plugins::guarded_d3d12_event(subscription->callback, client.self, event, data, subscription->user, fault);
        const auto elapsed = static_cast<std::uint64_t>(qpc() - start);
        subscription->calls[event].fetch_add(1, std::memory_order_relaxed);
        subscription->ticks[event].fetch_add(elapsed, std::memory_order_relaxed);
        if (elapsed > subscription->max_ticks[event].load(std::memory_order_relaxed)) {
            subscription->max_ticks[event].store(elapsed, std::memory_order_relaxed);
        }
        if (!returned && client.disable) {
            client.disable(std::string("its ") + event_name(event) + " callback raised exception " +
                           std::to_string(fault));
        }
    }
}

std::vector<std::string> Hub::summary() const {
    std::vector<std::string> lines;
    std::lock_guard lock(mutex_);
    char text[256]{};
    for (const auto& subscription : subscriptions_) {
        for (std::uint32_t event = 0; event < AL_D3D12_EVENT_COUNT; ++event) {
            if (!(subscription->events & AL_D3D12_EVENT_BIT(event))) continue;
            const auto calls = subscription->calls[event].load(std::memory_order_relaxed);
            const auto total = ticks_to_us(subscription->ticks[event].load(std::memory_order_relaxed));
            std::snprintf(text, sizeof(text), "d3d12_event plugin=%s event=%s order=%d calls=%llu total_us=%.1f "
                          "mean_us=%.2f max_us=%.1f", subscription->client->name.c_str(), event_name(event),
                          subscription->order, static_cast<unsigned long long>(calls), total,
                          calls ? total / static_cast<double>(calls) : 0.0,
                          ticks_to_us(subscription->max_ticks[event].load(std::memory_order_relaxed)));
            lines.emplace_back(text);
        }
    }
    for (const auto& chain : chains_) {
        std::string line = "d3d12_chain method=" + method_name(chain.method) + " layers=";
        for (std::size_t k = 0; k < chain.layers.size(); ++k) {
            line += (k ? "," : "") + (chain.layers[k].client ? chain.layers[k].client->name : std::string("astera"));
        }
        line += std::string(" installed=") +
                (installed_[AL_D3D12_METHOD_FAMILY(chain.method)] ? "yes" : "no");
        lines.push_back(std::move(line));
    }
    lines.push_back(std::string("d3d12_state=") +
                    (state_ == AL_D3D12_RUNNING ? "running" : state_ == AL_D3D12_WAITING ? "waiting"
                                                                                             : "unavailable") +
                    " presentation_owner=" + (owner_ ? owner_name_ : std::string("none")) +
                    " frames=" + std::to_string(frames_.load()) +
                    (state_ == AL_D3D12_UNAVAILABLE ? " reason=" + reason_ : std::string()));
    return lines;
}

}  // namespace al::graphics
